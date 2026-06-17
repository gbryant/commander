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

    // B1: a SECOND command session (CH_TOOLS=2) dispatches independently and frames its
    // reply back on ITS channel — the multi-session capability, isolated from ch0.
    g_tx.clear(); feedFrame(ct,CH_TOOLS,"help");
    uint8_t tch=255; std::string tout;
    bool ok_sess = deframe(g_tx,tch,tout) && tch==CH_TOOLS && tout.find("help")!=std::string::npos;
    printf("%s ch%u (CH_TOOLS) command session 'help' -> framed response on ch%u\n",
           ok_sess?"PASS":"FAIL", CH_TOOLS, tch); fails+=!ok_sess;

    // role check straight from the authority table
    bool ok_role = channel_is_command_session(CH_CONSOLE) && channel_is_command_session(CH_TOOLS)
                   && !channel_is_command_session(CH_IR);
    printf("%s channel_ids: console+tools are sessions, ir is data\n", ok_role?"PASS":"FAIL"); fails+=!ok_role;

    feedFrame(ct,5,"hello-data");
    bool ok2 = (got_ch5==1 && ch5_payload=="hello-data");
    printf("%s ch5 subscriber received '%s'\n", ok2?"PASS":"FAIL", ch5_payload.c_str()); fails+=!ok2;

    g_tx.clear(); ct.publishStr(2,"sensor=42");
    uint8_t pch=255; std::string pp;
    bool ok3 = deframe(g_tx,pch,pp) && pch==2 && pp=="sensor=42";
    printf("%s publish ch2 -> framed '%s'\n", ok3?"PASS":"FAIL", pp.c_str()); fails+=!ok3;

    // ChannelPublisher (the module publish API): a Writer bound to a channel, one line = one frame
    g_tx.clear();
    ChannelTransport::ChannelPublisher pub = ct.publisher(7);
    bool ok_valid = pub.valid() && pub.channel()==7;
    printf("%s publisher(7) is valid, bound to ch7\n", ok_valid?"PASS":"FAIL"); fails+=!ok_valid;

    pub.writeln("ir:0xA90");                 // one event -> one frame (no \r\n: the frame IS the delim)
    uint8_t wch=255; std::string wp;
    bool ok5 = deframe(g_tx,wch,wp) && wch==7 && wp=="ir:0xA90";
    printf("%s publisher writeln -> one framed event on ch7 ('%s')\n", ok5?"PASS":"FAIL", wp.c_str()); fails+=!ok5;

    // buffered write() accumulates, writeln() flushes the whole line as a single frame
    g_tx.clear(); pub.write("a="); pub.write("1"); pub.writeln();
    ChannelReader pr; int pn=0; std::string plast;
    for (auto b: g_tx) if (pr.feed(b)) { pn++; plast.assign((const char*)pr.payload(), pr.len()); }
    bool ok6 = (pn==1 && plast=="a=1");
    printf("%s publisher write()x2 + writeln() -> single frame '%s'\n", ok6?"PASS":"FAIL", plast.c_str()); fails+=!ok6;

    // default-constructed publisher is inert (valid()==false, no output)
    g_tx.clear();
    ChannelTransport::ChannelPublisher none;
    none.publishStr("dropped"); none.writeln("dropped");
    bool ok7 = (!none.valid() && g_tx.empty());
    printf("%s default publisher is inert (no frames)\n", ok7?"PASS":"FAIL"); fails+=!ok7;

    // console + publish don't cross-talk: a ch0 command, then a ch1 publish, both decode distinctly
    g_tx.clear(); feedFrame(ct,0,"version"); ct.publishStr(1,"ir:0xA5");
    ChannelReader r; int n=0; uint8_t chs[8]={0};
    for (auto b: g_tx) if (r.feed(b)) chs[n++]=r.channel();
    bool ok4 = (n==2 && chs[0]==0 && chs[1]==1);
    printf("%s ch0 response + ch1 publish interleave cleanly (%d frames)\n", ok4?"PASS":"FAIL", n); fails+=!ok4;

    printf(fails?"\n%d FAILED\n":"\nALL PASS\n", fails);
    return fails;
}
