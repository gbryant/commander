// Host test for the COBS channel frame codec (transport/channels/ChannelCodec.h).
// Build+run via transport/channels/tests/run.sh.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#define CMDR_CH_FRAME_MAX 1024
#include "transport/channels/ChannelCodec.h"

static int fails = 0;
static void check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

// encode a frame, feed it (optionally with leading garbage) through a reader, verify decode
static bool roundtrip(uint8_t ch, const uint8_t* pl, size_t len, const char* name, bool noise=false) {
    uint8_t out[2048];
    size_t n = channel_encode(ch, pl, len, out);
    // the encoded body (minus delimiter) must contain no 0x00
    for (size_t i = 0; i + 1 < n; i++) if (out[i]==0) { printf("  (zero in body!)\n"); return false; }
    ChannelReader r;
    bool got = false;
    if (noise) { r.feed(0x05); r.feed(0xAA); r.feed(0xBB); r.feed(0x00); } // garbage frame + delimiter -> dropped, then clean
    for (size_t i = 0; i < n; i++) if (r.feed(out[i])) got = true;
    if (!got) { printf("  (no frame for %s)\n", name); return false; }
    if (r.channel()!=ch || r.len()!=len || memcmp(r.payload(), pl, len)!=0) {
        printf("  (mismatch %s: ch=%u len=%zu)\n", name, r.channel(), r.len()); return false;
    }
    return true;
}

int main() {
    uint8_t a[] = {1,2,3,4,5};
    check(roundtrip(7, a, sizeof(a), "simple"), "simple frame");

    uint8_t z[] = {0,1,0,0,2,0};                 // payload full of zeros (COBS stress)
    check(roundtrip(3, z, sizeof(z), "zeros"), "payload with 0x00 bytes");

    check(roundtrip(0, nullptr, 0, "empty"), "empty payload (console-style)");

    std::vector<uint8_t> big(600);               // > 254 -> forces a 0xFF COBS run
    for (size_t i=0;i<big.size();i++) big[i]=(uint8_t)(i%251)+1; // no zeros
    check(roundtrip(9, big.data(), big.size(), "big-nozero"), "600B no-zero (0xFF run)");

    std::vector<uint8_t> bigz(600);              // > 254 with zeros sprinkled
    for (size_t i=0;i<bigz.size();i++) bigz[i]=(uint8_t)(i%7==0?0:i);
    check(roundtrip(9, bigz.data(), bigz.size(), "big-zeros"), "600B with zeros");

    check(roundtrip(7, a, sizeof(a), "noise", true), "resync after line noise");

    // two back-to-back frames in one stream
    {
        uint8_t out[256]; size_t n1=channel_encode(1,a,sizeof(a),out);
        size_t n2=channel_encode(2,z,sizeof(z),out+n1);
        ChannelReader r; int frames=0; uint8_t chans[4]={0};
        for (size_t i=0;i<n1+n2;i++) if (r.feed(out[i])) chans[frames++]=r.channel();
        check(frames==2 && chans[0]==1 && chans[1]==2, "two frames back-to-back");
    }
    printf(fails? "\n%d FAILED\n":"\nALL PASS\n", fails);
    return fails;
}
