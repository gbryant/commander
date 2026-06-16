// Stateless codec filter for the codec<->broker byte-compat guard (test_codec_compat.py).
// The MCU-side COBS codec (ChannelCodec.h) and the Python broker's port
// (commander_broker.py) drifting apart is a SILENT failure — mismatched frames just
// vanish. This harness exposes the real C codec as a Unix filter so the Python test
// can drive a byte-for-byte round-trip in both directions. The test vectors live only
// on the Python side; this stays a pure codec, no embedded expectations.
//
//   encode:  stdin = "<ch> <hexpayload>\n" lines  -> stdout = raw channel_encode() bytes
//   decode:  stdin = raw COBS frame bytes          -> stdout = "<ch> <hexpayload>\n" lines
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#define CMDR_CH_FRAME_MAX 4096
#include "transport/channels/ChannelCodec.h"

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int do_encode() {
    // Read all of stdin, split into lines "<ch> <hex>".
    std::string all;
    int c;
    while ((c = getchar()) != EOF) all.push_back((char)c);
    size_t i = 0;
    std::vector<uint8_t> outbuf;
    while (i < all.size()) {
        size_t nl = all.find('\n', i);
        if (nl == std::string::npos) nl = all.size();
        std::string line = all.substr(i, nl - i);
        i = nl + 1;
        if (line.empty()) continue;
        size_t sp = line.find(' ');
        int ch = atoi(line.substr(0, sp).c_str());
        std::vector<uint8_t> pl;
        if (sp != std::string::npos) {
            std::string hex = line.substr(sp + 1);
            for (size_t k = 0; k + 1 < hex.size(); k += 2) {
                int hi = hexval(hex[k]), lo = hexval(hex[k + 1]);
                if (hi < 0 || lo < 0) break;
                pl.push_back((uint8_t)((hi << 4) | lo));
            }
        }
        uint8_t enc[CMDR_CH_FRAME_MAX * 2];
        size_t n = channel_encode((uint8_t)ch, pl.data(), pl.size(), enc);
        outbuf.insert(outbuf.end(), enc, enc + n);
    }
    fwrite(outbuf.data(), 1, outbuf.size(), stdout);
    return 0;
}

static int do_decode() {
    ChannelReader r;
    int c;
    while ((c = getchar()) != EOF) {
        if (r.feed((uint8_t)c)) {
            printf("%u ", (unsigned)r.channel());
            const uint8_t *p = r.payload();
            for (size_t k = 0; k < r.len(); k++) printf("%02x", p[k]);
            printf("\n");
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: codec_harness encode|decode\n"); return 2; }
    if (strcmp(argv[1], "encode") == 0) return do_encode();
    if (strcmp(argv[1], "decode") == 0) return do_decode();
    fprintf(stderr, "unknown mode '%s'\n", argv[1]);
    return 2;
}
