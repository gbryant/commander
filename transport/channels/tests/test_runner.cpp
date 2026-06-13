// Host test for the channel-bus runner (transport/channels/ChannelBusRunner.{h,cpp}).
// Compiles ChannelBusRunner.cpp against a tiny HAL stub (below) and exercises the
// begin() wiring: the weak commander_on_channels_ready() hook must fire, and a publish
// through the bus must reach the byte writer (hal_uart_putchar) as a decodable frame.
// We do NOT run taskBody() (it loops forever); the RX path is covered by test_transport.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "transport/channels/ChannelBusRunner.h"

// ── HAL stub ────────────────────────────────────────────────────────────────────
static std::vector<uint8_t> g_uart_out;
extern "C" {
    void hal_uart_init(uint32_t) {}
    void hal_uart_putchar(char c) { g_uart_out.push_back((uint8_t)c); }
    void hal_uart_puts(const char *s) { while (*s) g_uart_out.push_back((uint8_t)*s++); }
    int  hal_uart_getchar(uint32_t) { return -1; }
    void hal_delay_ms(uint32_t) {}
    uint64_t hal_time_us(void) { return 0; }
}

// ── App hook: stash the transport the runner hands us ────────────────────────────
static ChannelTransport *g_ready = nullptr;
extern "C" void commander_on_channels_ready(ChannelTransport &ct) { g_ready = &ct; }

static bool deframe(std::vector<uint8_t> &tx, uint8_t &ch, std::string &payload) {
    ChannelReader r; bool got = false;
    for (auto b : tx) if (r.feed(b)) { ch = r.channel(); payload.assign((const char *)r.payload(), r.len()); got = true; }
    return got;
}

int main() {
    CommandRegistry reg; SystemModule sys; reg.registerModule(sys);
    ChannelBusRunner runner;
    int fails = 0;

    runner.begin(reg);   // no-baud overload -> no hal_uart_init needed
    bool ok1 = (g_ready == &runner.channels());
    printf("%s commander_on_channels_ready fired with the runner's transport\n", ok1 ? "PASS" : "FAIL"); fails += !ok1;

    g_uart_out.clear();
    runner.channels().publisher(3).writeln("beat 1");
    uint8_t ch = 255; std::string p;
    bool ok2 = deframe(g_uart_out, ch, p) && ch == 3 && p == "beat 1";
    printf("%s publish via runner reaches hal_uart_putchar as a ch3 frame ('%s')\n", ok2 ? "PASS" : "FAIL", p.c_str()); fails += !ok2;

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails;
}
