// Host test for the channel-mux transport (transport/channels/ChannelTransport.h).
// Build+run via transport/channels/tests/run.sh.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "transport/channels/ChannelTransport.h"

static std::vector<uint8_t> g_tx;
static void capture(const uint8_t* d, size_t n, void*) { g_tx.insert(g_tx.end(), d, d+n); }
static int got_ch5=0; static std::string ch5_payload;
static void onCh5(uint8_t, const uint8_t* d, size_t n, void*) { got_ch5++; ch5_payload.assign((const char*)d,n); }

static void feedFrame(ChannelTransport& t, uint8_t ch, const char* s) {
    uint8_t enc[512]; size_t n=channel_encode(ch,(const uint8_t*)s,strlen(s),enc);
    for (size_t i=0;i<n;i++) t.feedByte(enc[i]);
}
static bool deframe(std::vector<uint8_t>& tx, uint8_t& ch, std::string& payload) {
    ChannelReader r; bool got=false;
    for (auto b: tx) if (r.feed(b)) { ch=r.channel(); payload.assign((const char*)r.payload(), r.len()); got=true; }
    return got;
}

int main() {
    CommandRegistry reg; SystemModule sys; reg.registerModule(sys);
    ChannelTransport ct(reg, capture, nullptr);
    ct.subscribe(5, onCh5, nullptr);
    int fails=0;

    g_tx.clear(); feedFrame(ct,0,"help");
    uint8_t ch=255; std::string out;
    bool ok1 = deframe(g_tx,ch,out) && ch==0 && out.find("help")!=std::string::npos && out.find("version")!=std::string::npos;
    printf("%s ch0 console 'help' -> framed response on ch0\n", ok1?"PASS":"FAIL"); fails+=!ok1;

    feedFrame(ct,5,"hello-data");
    bool ok2 = (got_ch5==1 && ch5_payload=="hello-data");
    printf("%s ch5 subscriber received '%s'\n", ok2?"PASS":"FAIL", ch5_payload.c_str()); fails+=!ok2;

    g_tx.clear(); ct.publishStr(2,"sensor=42");
    uint8_t pch=255; std::string pp;
    bool ok3 = deframe(g_tx,pch,pp) && pch==2 && pp=="sensor=42";
    printf("%s publish ch2 -> framed '%s'\n", ok3?"PASS":"FAIL", pp.c_str()); fails+=!ok3;

    // console + publish don't cross-talk: a ch0 command, then a ch1 publish, both decode distinctly
    g_tx.clear(); feedFrame(ct,0,"version"); ct.publishStr(1,"ir:0xA5");
    ChannelReader r; int n=0; uint8_t chs[8]={0};
    for (auto b: g_tx) if (r.feed(b)) chs[n++]=r.channel();
    bool ok4 = (n==2 && chs[0]==0 && chs[1]==1);
    printf("%s ch0 response + ch1 publish interleave cleanly (%d frames)\n", ok4?"PASS":"FAIL", n); fails+=!ok4;

    printf(fails?"\n%d FAILED\n":"\nALL PASS\n", fails);
    return fails;
}
