#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include "hal/hal.h"

// SSCMA / Seeed SenseCraft Model Assistant client — the protocol the Grove Vision
// AI Module V2 (WiseEye2) speaks to a host MCU. Clean-room port of the framing in
// Seeed_Arduino_SSCMA (that library is Arduino/Wire/ArduinoJson-bound; this is not).
//
// The wire protocol is transport-agnostic: identical ASCII AT commands and JSON
// responses ride over UART or I2C — only the byte plumbing differs. That seam is
// ISscmaTransport (txWrite/rxAvailable/rxRead); SscmaClient layers the AT command
// builder + a non-blocking response parser on top. See modules/aicam/I2cTransport.h
// (portable) and platform/esp32/AiCamUartTransport.h (esp32) for the two backends.
//
//   Request : "AT+<CMD>=<args>\r\n"
//   Response: "\r{json}\n"  with {"type":0|1|2,"name":..,"code":..,"data":..}
//             type 0 = command response, 1 = event (carries inference results),
//             2 = logging. INVOKE/SAMPLE events carry integer result arrays:
//               boxes  [[x,y,w,h,score,target], ...]
//               classes[[score,target], ...]
//               points [[x,y,score,target], ...]
//               perf   [preprocess_ms, inference_ms, postprocess_ms]
//             plus, for SAMPLE / non-result_only INVOKE, a base64 "image".

#ifndef AICAM_RX_MAX
// RX accumulator. Big enough for a small/medium base64 JPEG (snap) plus headroom;
// override for larger frames. Detection ("result only") frames are well under 1 KB.
#define AICAM_RX_MAX 24576
#endif
#ifndef AICAM_FRAME_MAX
#define AICAM_FRAME_MAX 1024   // copy buffer for a parsed result frame (no image)
#endif

// ── Result types (fixed-size — no std::vector, matching the RAM-tight ethos) ──
struct AiBox   { uint16_t x, y, w, h; uint8_t score, target; };
struct AiClass { uint8_t score, target; };
struct AiPoint { uint16_t x, y; uint8_t score, target; };
struct AiPerf  { uint16_t preprocess, inference, postprocess; };

static constexpr int kAiMaxRows = 12;   // max detections kept per frame

struct AiResult {
    AiBox   boxes[kAiMaxRows];   uint8_t nBoxes   = 0;
    AiClass classes[kAiMaxRows]; uint8_t nClasses = 0;
    AiPoint points[kAiMaxRows];  uint8_t nPoints  = 0;
    AiPerf  perf = {0, 0, 0};
};

// ── Transport seam ───────────────────────────────────────────────────────────
struct ISscmaTransport {
    virtual ~ISscmaTransport() = default;
    virtual void begin()                             {}     // bring up the bus (once)
    virtual void txWrite(const char *data, int len) = 0;
    virtual int  rxAvailable()                       = 0;   // bytes ready to read
    virtual int  rxRead(char *data, int len)         = 0;   // up to len; returns count
};

class SscmaClient {
public:
    explicit SscmaClient(ISscmaTransport &t) : _t(t) {}

    // Send "AT+<body>\r\n". `body` is everything after the AT+ prefix, e.g. "ID?".
    void writeAt(const char *body) {
        char cmd[192];
        int n = snprintf(cmd, sizeof(cmd), "AT+%s\r\n", body);
        if (n > 0) _t.txWrite(cmd, n);
    }
    // Send a raw line as typed (AT passthrough); appends CRLF.
    void writeRaw(const char *line) {
        char cmd[192];
        int n = snprintf(cmd, sizeof(cmd), "%s\r\n", line);
        if (n > 0) _t.txWrite(cmd, n);
    }

    // Blocking: send `body`, return the first response frame (JSON, braces incl.)
    // in `out`. True if a frame arrived before timeout.
    bool request(const char *body, char *out, int cap, uint32_t timeout_ms) {
        flush();
        writeAt(body);
        return readFrame(out, cap, timeout_ms);
    }

    // Drain buffered input + reset the accumulator (before issuing a new command).
    void flush() {
        _accLen = 0;
        for (int i = 0; i < 16; i++) {
            int a = _t.rxAvailable();
            if (a <= 0) break;
            char tmp[64];
            _t.rxRead(tmp, a < (int)sizeof(tmp) ? a : (int)sizeof(tmp));
        }
    }

    // Pump + pop the next complete response frame within timeout (no write).
    bool readFrame(char *out, int cap, uint32_t timeout_ms) {
        uint64_t t0 = hal_time_us();
        do {
            pump(5);
            if (extractFrame(out, cap) > 0) return true;
            hal_delay_ms(2);
        } while (elapsed_ms(t0) < timeout_ms);
        return false;
    }

    // Blocking: run inference (`body` = the INVOKE arg string) and capture the
    // first *event* frame's results. Skips the type-0 ack.
    bool invokeEvent(const char *body, AiResult &res, uint32_t timeout_ms) {
        flush();
        writeAt(body);
        return nextEvent(res, timeout_ms);
    }

    // Read the next inference event (type 1) into `res`, skipping ack/log frames.
    // No write — pairs with a single writeAt("INVOKE=n,...") to drain all n events.
    bool nextEvent(AiResult &res, uint32_t timeout_ms) {
        char frame[AICAM_FRAME_MAX];
        uint64_t t0 = hal_time_us();
        do {
            pump(5);
            int len;
            while ((len = extractFrame(frame, sizeof(frame))) > 0) {
                if (frameInt(frame, "type") == 1) { parseResult(frame, res); return true; }
            }
            hal_delay_ms(2);
        } while (elapsed_ms(t0) < timeout_ms);
        return false;
    }

    // Non-blocking poll for streaming mode (after starting AT+INVOKE=-1). Drains
    // whatever is ready; if a complete event frame is present, parses it into
    // `res` and returns true. Call repeatedly from a ticker.
    bool poll(AiResult &res) {
        pump(2);
        char frame[AICAM_FRAME_MAX];
        int len;
        while ((len = extractFrame(frame, sizeof(frame))) > 0) {
            if (frameInt(frame, "type") == 1) { parseResult(frame, res); return true; }
        }
        return false;
    }

    // Capture one frame's base64 image (via AT+SAMPLE=1). Copies up to dstCap-1
    // bytes of base64 into `dst` (NUL-terminated); `outLen` is the *full* image
    // length (so a value > dstCap means it was truncated / raise AICAM_RX_MAX).
    bool captureImage(char *dst, int dstCap, int &outLen, uint32_t timeout_ms) {
        flush();
        writeAt("SAMPLE=1");
        outLen = 0;
        uint64_t t0 = hal_time_us();
        while (elapsed_ms(t0) < timeout_ms) {
            pump(5);
            if (scanImage(dst, dstCap, outLen)) return true;
            hal_delay_ms(2);
        }
        return false;
    }

    // Extract a string value from a response frame's data, e.g. the ID/name/version
    // returned by AT+ID? etc. Returns true and copies into `out` (without quotes).
    static bool frameStr(const char *json, const char *key, char *out, int cap) {
        const char *p = findVal(json, key);
        if (!p) return false;
        p = skipWs(p);
        if (*p == ':') { ++p; p = skipWs(p); }
        if (*p != '"') return false;
        ++p;
        int n = 0;
        while (*p && *p != '"' && n < cap - 1) { out[n++] = *p++; }
        out[n] = '\0';
        return true;
    }
    static int frameInt(const char *json, const char *key) {
        const char *p = findVal(json, key);
        if (!p) return -1;
        return rdInt(p);
    }

private:
    ISscmaTransport &_t;
    char     _acc[AICAM_RX_MAX];
    int      _accLen = 0;

    static uint32_t elapsed_ms(uint64_t t0) { return (uint32_t)((hal_time_us() - t0) / 1000); }

    // Pull ready bytes into the accumulator for up to budget_ms. Returns bytes read.
    int pump(uint32_t budget_ms) {
        uint64_t t0 = hal_time_us();
        int total = 0;
        do {
            int avail = _t.rxAvailable();
            if (avail <= 0) break;
            int space = AICAM_RX_MAX - _accLen;
            if (space <= 0) {                       // overflow: drop the oldest half
                int drop = AICAM_RX_MAX / 2;
                memmove(_acc, _acc + drop, _accLen - drop);
                _accLen -= drop;
                space = AICAM_RX_MAX - _accLen;
            }
            int want = avail < space ? avail : space;
            int got = _t.rxRead(_acc + _accLen, want);
            if (got <= 0) break;
            _accLen += got;
            total   += got;
        } while (elapsed_ms(t0) < budget_ms);
        return total;
    }

    // Pop one complete "\r{...}\n" frame from the accumulator. Returns the full
    // frame length (0 if none yet); copies up to cap-1 bytes of {...} into `out`.
    int extractFrame(char *out, int cap) {
        int start = -1;
        for (int i = 0; i + 1 < _accLen; i++)
            if (_acc[i] == '\r' && _acc[i + 1] == '{') { start = i + 1; break; }
        if (start < 0) {
            // No frame start; keep only a trailing partial '\r' so we don't grow forever.
            if (_accLen > 1) { _acc[0] = _acc[_accLen - 1]; _accLen = 1; }
            return 0;
        }
        int depth = 0, end = -1;
        bool inStr = false, esc = false;
        for (int i = start; i < _accLen; i++) {
            char c = _acc[i];
            if (inStr) {
                if (esc)            esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"')  inStr = false;
                continue;
            }
            if (c == '"')      inStr = true;
            else if (c == '{') depth++;
            else if (c == '}') { if (--depth == 0) { end = i; break; } }
        }
        if (end < 0) {                              // frame still incomplete
            if (start > 1) { memmove(_acc, _acc + start - 1, _accLen - (start - 1)); _accLen -= (start - 1); }
            return 0;
        }
        int full = end - start + 1;
        int n = full < cap - 1 ? full : cap - 1;
        memcpy(out, _acc + start, n);
        out[n] = '\0';
        int consume = end + 1;
        if (consume < _accLen && _acc[consume] == '\n') consume++;
        memmove(_acc, _acc + consume, _accLen - consume);
        _accLen -= consume;
        return full;
    }

    // Find the base64 image value `"image": "<...>"` in the accumulator and, once
    // its closing quote has arrived, copy it out and consume it. Tolerates the
    // whitespace the firmware emits after the colon (`"image": "..."`).
    bool scanImage(char *dst, int dstCap, int &outLen) {
        static const char key[] = "\"image\"";
        char *limit = _acc + _accLen;
        char *k = (char *)memmem_(_acc, _accLen, key, sizeof(key) - 1);
        if (!k) return false;
        char *p = k + (sizeof(key) - 1);
        while (p < limit && (*p == ' ' || *p == '\t' || *p == ':')) ++p;
        if (p >= limit) return false;               // value not arrived yet
        if (*p != '"') return false;                // not a string value
        char *b = p + 1;                            // first base64 byte
        char *end = nullptr;
        for (char *q = b; q < limit; q++) if (*q == '"') { end = q; break; }
        if (!end) return false;                     // not fully received yet
        outLen = (int)(end - b);
        int n = outLen < dstCap - 1 ? outLen : dstCap - 1;
        if (dst && dstCap > 0) { memcpy(dst, b, n); dst[n] = '\0'; }
        _accLen = 0;                                // image frame consumed; reset
        return true;
    }

    // ── parsing helpers ──────────────────────────────────────────────────────
    static const char *skipWs(const char *p) { while (*p == ' ' || *p == '\t') ++p; return p; }
    static int rdInt(const char *&p) {
        p = skipWs(p);
        if (*p == ':') { ++p; p = skipWs(p); }
        int s = 1;
        if (*p == '-') { s = -1; ++p; }
        int v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
        return s * v;
    }
    // Pointer just past `"key"` (at the ':'), or null.
    static const char *findVal(const char *json, const char *key) {
        char pat[24];
        snprintf(pat, sizeof(pat), "\"%s\"", key);
        const char *p = strstr(json, pat);
        return p ? p + strlen(pat) : nullptr;
    }
    // Pointer at the opening '[' of array field `key`, or null.
    static const char *findArr(const char *json, const char *key) {
        const char *p = findVal(json, key);
        if (!p) return nullptr;
        p = skipWs(p);
        if (*p == ':') { ++p; p = skipWs(p); }
        return *p == '[' ? p : nullptr;
    }
    // Parse "[[a,b,..],[..]]" into row-major out[]; returns row count.
    static uint8_t parseMatrix(const char *p, int cols, int *out, int maxRows) {
        if (*p != '[') return 0;
        ++p;
        uint8_t rows = 0;
        while (*p && *p != ']' && rows < maxRows) {
            p = skipWs(p);
            if (*p != '[') break;
            ++p;
            for (int c = 0; c < cols; c++) {
                out[rows * cols + c] = rdInt(p);
                p = skipWs(p);
                if (*p == ',') ++p;
            }
            while (*p && *p != ']') ++p;
            if (*p == ']') ++p;
            rows++;
            p = skipWs(p);
            if (*p == ',') ++p;
        }
        return rows;
    }
    // Parse a flat "[a,b,c]" into out[]; returns count.
    static uint8_t parseFlat(const char *p, int *out, int maxN) {
        if (*p != '[') return 0;
        ++p;
        uint8_t n = 0;
        while (*p && *p != ']' && n < maxN) {
            out[n++] = rdInt(p);
            p = skipWs(p);
            if (*p == ',') ++p;
            p = skipWs(p);
        }
        return n;
    }
    static void parseResult(const char *frame, AiResult &res) {
        res.nBoxes = res.nClasses = res.nPoints = 0;
        res.perf = {0, 0, 0};
        const char *p;
        if ((p = findArr(frame, "boxes"))) {
            int m[kAiMaxRows * 6];
            uint8_t r = parseMatrix(p, 6, m, kAiMaxRows);
            res.nBoxes = r;
            for (int i = 0; i < r; i++)
                res.boxes[i] = { (uint16_t)m[i*6], (uint16_t)m[i*6+1], (uint16_t)m[i*6+2],
                                 (uint16_t)m[i*6+3], (uint8_t)m[i*6+4], (uint8_t)m[i*6+5] };
        }
        if ((p = findArr(frame, "classes"))) {
            int m[kAiMaxRows * 2];
            uint8_t r = parseMatrix(p, 2, m, kAiMaxRows);
            res.nClasses = r;
            for (int i = 0; i < r; i++)
                res.classes[i] = { (uint8_t)m[i*2], (uint8_t)m[i*2+1] };
        }
        if ((p = findArr(frame, "points"))) {
            int m[kAiMaxRows * 4];
            uint8_t r = parseMatrix(p, 4, m, kAiMaxRows);
            res.nPoints = r;
            for (int i = 0; i < r; i++)
                res.points[i] = { (uint16_t)m[i*4], (uint16_t)m[i*4+1],
                                  (uint8_t)m[i*4+2], (uint8_t)m[i*4+3] };
        }
        if ((p = findArr(frame, "perf"))) {
            int m[4];
            uint8_t r = parseFlat(p, m, 4);
            if (r >= 3) res.perf = { (uint16_t)m[0], (uint16_t)m[1], (uint16_t)m[2] };
        }
    }

    // memmem isn't standard everywhere (AVR libc lacks it); small local version.
    static void *memmem_(const void *hay, size_t hayLen, const void *ndl, size_t ndlLen) {
        if (ndlLen == 0 || hayLen < ndlLen) return nullptr;
        const char *h = (const char *)hay;
        for (size_t i = 0; i + ndlLen <= hayLen; i++)
            if (memcmp(h + i, ndl, ndlLen) == 0) return (void *)(h + i);
        return nullptr;
    }
};
