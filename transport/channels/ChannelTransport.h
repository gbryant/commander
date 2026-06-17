#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "core/CommandRegistry.h"
#include "core/Writer.h"
#include "transport/channels/ChannelCodec.h"
#include "channel_ids.h"

// Channel-mux transport — the peer pub/sub bus between commander (MCU) and a host/SBC
// broker over one byte link (see docs/commander-channels-design.md). Frames are
// COBS([channel][payload]) (ChannelCodec.h). Channels are multiplexed both directions:
//
//   command sessions — channels flagged command_session in channel_ids.h (ch0 console,
//                   ch2 tools, …). Inbound payload is a complete command line; we
//                   dispatch it through the CommandRegistry and frame the output back on
//                   THAT SAME channel, so several host processes each get an isolated
//                   shell over the one link (roadmap #2). Line editing / echo is the
//                   broker's job, not the MCU's — the bus is message-oriented.
//   data channels — delivered to a subscribed handler; modules publish() unsolicited.
//
// Byte I/O is injected (WriteFn out, feedByte() in) so this is host-testable and link-
// agnostic; the runner wires feedByte() to hal_uart_getchar and WriteFn to a byte writer.
// (Frames contain 0x00 delimiters, so output must be a byte writer, NOT hal_uart_puts.)
class ChannelTransport {
public:
    // Channel ids/roles live in channel_ids.h (the authority). Kept here as an alias for
    // the long-standing ChannelTransport::CH_CONSOLE spelling.
    static constexpr uint8_t CH_CONSOLE = ::CH_CONSOLE;

    typedef void (*WriteFn)(const uint8_t *data, size_t len, void *ctx);
    typedef void (*Handler)(uint8_t ch, const uint8_t *data, size_t len, void *ctx);

    ChannelTransport() = default;
    ChannelTransport(CommandRegistry &reg, WriteFn wr, void *wrCtx)
        : _reg(&reg), _wr(wr), _wrCtx(wrCtx) {}

    // Late init for the default-constructed-then-begin() idiom (so a runner can hold one
    // as a member and wire it once the registry + byte writer exist).
    void begin(CommandRegistry &reg, WriteFn wr, void *wrCtx) {
        _reg = &reg; _wr = wr; _wrCtx = wrCtx;
    }

    // Drive from the read loop, one inbound byte at a time.
    void feedByte(uint8_t b) {
        if (_reader.feed(b)) route(_reader.channel(), _reader.payload(), _reader.len());
    }

    // Publish an unsolicited message on a channel (the new peer capability).
    void publish(uint8_t ch, const uint8_t *data, size_t len) {
        if (!_wr) return;                                              // not begun → no link
        uint8_t enc[CMDR_CH_FRAME_MAX + CMDR_CH_FRAME_MAX / 254 + 4];
        if (len > CMDR_CH_FRAME_MAX - 1) len = CMDR_CH_FRAME_MAX - 1;   // bound to one frame
        size_t n = channel_encode(ch, data, len, enc);
        _wr(enc, n, _wrCtx);
    }
    void publishStr(uint8_t ch, const char *s) { publish(ch, (const uint8_t *)s, strlen(s)); }

    // Register a handler for inbound frames on a (non-console) channel.
    bool subscribe(uint8_t ch, Handler h, void *ctx) {
        if (_nsub >= kMaxSub) return false;
        _sub[_nsub++] = {ch, h, ctx};
        return true;
    }

    // A per-channel publish handle a module/app holds to emit UNSOLICITED frames — the
    // module publish API (the doc's `channel("ir").publish(...)`). IS-A Writer so an
    // async module that already emits via Writer& (IR recv, a sensor stream) frames each
    // line onto its OWN channel instead of the single console: line-oriented, so each
    // writeln() flushes one frame = one event. Raw publish() carries binary payloads.
    // Default-constructed (valid()==false) it no-ops, so a module can hold one unwired
    // until the app fills it in from commander_on_channels_ready().
    class ChannelPublisher : public Writer {
    public:
        ChannelPublisher() : _t(nullptr), _ch(0) {}
        ChannelPublisher(ChannelTransport &t, uint8_t ch) : _t(&t), _ch(ch) {}
        bool    valid()   const { return _t != nullptr; }
        uint8_t channel() const { return _ch; }
        void publish(const uint8_t *data, size_t len) { if (_t) _t->publish(_ch, data, len); }
        void publishStr(const char *s)                { publish((const uint8_t *)s, strlen(s)); }
        void write(const char *s)        override { append(s); }
        void writeln(const char *s = "") override { append(s); flush(); }  // one line = one frame
        void flush() { if (_t && _n) { _t->publish(_ch, _buf, _n); _n = 0; } }
    private:
        void append(const char *s) {
            while (*s) { if (_n >= sizeof(_buf)) flush(); _buf[_n++] = (uint8_t)*s++; }
        }
        ChannelTransport *_t;
        uint8_t           _ch;
        uint8_t           _buf[CMDR_CH_FRAME_MAX - 8];
        size_t            _n = 0;
    };

    // Hand out a publisher bound to a channel (e.g. ct.publisher(CH_IR) wired to the IR
    // module in commander_on_channels_ready). Cheap value type — copy/store freely.
    ChannelPublisher publisher(uint8_t ch) { return ChannelPublisher(*this, ch); }

    // A Writer that frames command output back onto a channel (buffered, flushed per
    // command — or sooner if a single response overflows one frame).
    class ChannelWriter : public Writer {
    public:
        ChannelWriter(ChannelTransport &t, uint8_t ch) : _t(t), _ch(ch) {}
        void write(const char *s)              override { append(s); }
        void writeln(const char *s = "")       override { append(s); append("\r\n"); }
        void flush() { if (_n) { _t.publish(_ch, _buf, _n); _n = 0; } }
    private:
        void append(const char *s) {
            while (*s) {
                if (_n >= sizeof(_buf)) flush();
                _buf[_n++] = (uint8_t)*s++;
            }
        }
        ChannelTransport &_t;
        uint8_t _ch;
        uint8_t _buf[CMDR_CH_FRAME_MAX - 8];
        size_t  _n = 0;
    };

private:
    struct Sub { uint8_t ch; Handler h; void *ctx; };
    static constexpr uint8_t kMaxSub = 8;

    void route(uint8_t ch, const uint8_t *data, size_t len) {
        // A command session (ch0 console, ch2 tools, …) dispatches its inbound frame as a
        // command line and frames the reply back on the SAME channel — so each session is
        // an isolated shell. Everything else is data → subscribers. ch0 stays a session by
        // virtue of its channel_ids.h descriptor, so the console behaves exactly as before.
        if (channel_is_command_session(ch)) { dispatchOn(ch, data, len); return; }
        for (uint8_t i = 0; i < _nsub; i++)
            if (_sub[i].ch == ch && _sub[i].h) _sub[i].h(ch, data, len, _sub[i].ctx);
    }
    void dispatchOn(uint8_t ch, const uint8_t *data, size_t len) {
        char cmd[CMDR_CH_FRAME_MAX];
        size_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
        memcpy(cmd, data, n);
        cmd[n] = '\0';
        ChannelWriter out(*this, ch);     // reply frames back on the originating channel
        _reg->dispatch(cmd, out);
        out.flush();
    }

    CommandRegistry *_reg   = nullptr;
    WriteFn          _wr    = nullptr;
    void            *_wrCtx = nullptr;
    ChannelReader    _reader;
    Sub              _sub[kMaxSub];
    uint8_t          _nsub = 0;
};

// Weak app hook — the runner calls this (if defined) right after constructing the
// ChannelTransport on a bus build, handing the app/module the transport so it can
// publisher()/subscribe() (e.g. wire the IR module to publish on an `ir` channel).
// Unset → resolves to null and the runner skips the call; a strong app definition wins.
extern "C" __attribute__((weak)) void commander_on_channels_ready(ChannelTransport &);
