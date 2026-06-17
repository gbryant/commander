#pragma once
#include <stdint.h>

// ── Channel bus identity — the single authority for channel ids and roles ──────────
//
// This is to the channel bus what i2c_ids.h is to the I2C wire protocol: the ONE place
// channel integers and their roles are declared. The codegen (cmdr), the host broker
// (transport/channels/broker/commander_broker.py), and the SBC tools all derive from
// here — do NOT hand-pick a channel integer anywhere else, or the three drift and frames
// silently vanish. Mirror the table in the broker the way i2c_ids.h is mirrored across
// platforms (a CHANNELS list with the same ids/roles; keep it in sync).
//
// Only C++ channel-bus builds (today: the Uno Q) and the host tests include this; the
// UART-only boards never compile it, so it costs them nothing.

// ── Channel ids ────────────────────────────────────────────────────────────────────
// ch0 is reserved for the console binding (an invariant — see docs/channels-first-class.md).
inline constexpr uint8_t CH_CONSOLE = 0;   // human/host command console (a command session)
inline constexpr uint8_t CH_IR      = 1;   // IR press events (publish-only data)
inline constexpr uint8_t CH_TOOLS   = 2;   // a non-console command session for host-side
                                           // tools (e.g. the IR mapper) — lets a tool open
                                           // its OWN shell instead of reaching through ch0.

// ── Channel roles ───────────────────────────────────────────────────────────────────
enum ChannelDir  : uint8_t { CH_DIR_PUB = 1, CH_DIR_SUB = 2, CH_DIR_BOTH = 3 };  // MCU's view
enum ChannelKind : uint8_t { CH_KIND_TEXT = 0, CH_KIND_BINARY = 1 };

struct ChannelDesc {
    uint8_t     id;
    const char *name;
    uint8_t     dir;              // ChannelDir
    uint8_t     kind;             // ChannelKind
    bool        command_session;  // inbound frames are dispatched through the CommandRegistry
                                  // and the reply (plus any async stream) frames back here
};

// The table. Keep the broker's CHANNELS mirror in sync with this.
inline constexpr ChannelDesc CHANNEL_TABLE[] = {
    { CH_CONSOLE, "console", CH_DIR_BOTH, CH_KIND_TEXT, true  },
    { CH_IR,      "ir",      CH_DIR_PUB,  CH_KIND_TEXT, false },
    { CH_TOOLS,   "tools",   CH_DIR_BOTH, CH_KIND_TEXT, true  },
};
inline constexpr uint8_t CHANNEL_COUNT = sizeof(CHANNEL_TABLE) / sizeof(CHANNEL_TABLE[0]);

inline const ChannelDesc *channel_desc(uint8_t ch) {
    for (uint8_t i = 0; i < CHANNEL_COUNT; i++)
        if (CHANNEL_TABLE[i].id == ch) return &CHANNEL_TABLE[i];
    return nullptr;
}

// A channel is a command session if its descriptor says so. Unknown channels are not
// (an unsolicited data channel a module publishes to needs no table entry to be routed
// to subscribers — only command sessions must be declared).
inline bool channel_is_command_session(uint8_t ch) {
    const ChannelDesc *d = channel_desc(ch);
    return d && d->command_session;
}

inline const char *channel_name(uint8_t ch) {
    const ChannelDesc *d = channel_desc(ch);
    return d ? d->name : "?";
}
