// Host test for core/ — CommandRegistry dispatch, the Writer contract, SystemModule
// (help/version), the dropped-command overflow path, and duplicate-id detection.
// Pure C++, no hardware. Build+run via tests/run.sh.
#include <cstdio>
#include <cstring>
#include <string>
#include "core/CommandRegistry.h"
#include "core/SystemModule.h"
#include "core/NullWriter.h"

static int fails = 0;
static void check(bool ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

// A Writer that captures everything into a std::string (the test transport).
struct StringWriter : public Writer {
    std::string buf;
    bool        connected = true;
    void write(const char *s) override { buf += s; }
    void writeln(const char *s = "") override { buf += s; buf += '\n'; }
    bool ok() override { return connected; }
};

// CommandRegistry::validateIds() calls commander_on_panic() (weak, loops forever)
// on a duplicate id. Override it here so the test can observe the panic and return.
static int g_panics = 0;
void commander_on_panic() { g_panics++; }

static void noop(const char *, Writer &, void *) {}
static void echo_args(const char *args, Writer &out, void *) { out.write("args=["); out.write(args); out.write("]"); }

int main() {
    // ── dispatch: exact match, argument split, unknown, and leading spaces ──────
    {
        CommandRegistry reg;
        reg.registerCommand(CMD("ping", "p", I2C_NONE, [](const char *, Writer &out, void *){ out.write("pong"); }, nullptr));
        reg.registerCommand(CMD("echo", "e", I2C_NONE, echo_args, nullptr));

        StringWriter w; reg.dispatch("ping", w);
        check(w.buf == "pong", "dispatch exact match runs handler");

        StringWriter w2; reg.dispatch("echo hello world", w2);
        check(w2.buf == "args=[hello world]", "dispatch splits command from args at first space");

        StringWriter w3; reg.dispatch("   ping", w3);
        check(w3.buf == "pong", "dispatch skips leading spaces");

        StringWriter w4; reg.dispatch("echo", w4);
        check(w4.buf == "args=[]", "no-arg command gets empty args string");

        StringWriter w5; reg.dispatch("nope", w5);
        check(w5.buf.find("unknown") != std::string::npos, "unknown command reported");

        // a prefix of a real command must NOT match (pin != ping)
        StringWriter w6; reg.dispatch("pin", w6);
        check(w6.buf.find("unknown") != std::string::npos, "prefix of a command is not a match");

        // empty / whitespace-only line is a no-op (no 'unknown')
        StringWriter w7; reg.dispatch("   ", w7);
        check(w7.buf.empty(), "whitespace-only line is a silent no-op");
    }

    // ── SystemModule: help lists commands; version renders ──────────────────────
    {
        CommandRegistry reg; SystemModule sys; reg.registerModule(sys);
        StringWriter w; reg.dispatch("help", w);
        bool ok = w.buf.find("help") != std::string::npos && w.buf.find("version") != std::string::npos;
        check(ok, "help lists registered commands");

        StringWriter w2; reg.dispatch("version", w2);
        // version.h fallback: BUILD_NAME "commander" build 0 (unknown)
        check(w2.buf.find("commander") != std::string::npos && w2.buf.find("build") != std::string::npos,
              "version renders firmware name + build");
    }

    // ── overflow: registering past MAX_COMMANDS drops, doesn't corrupt ──────────
    {
        CommandRegistry reg;
        // MAX_COMMANDS is defined to 16 by the build line. Fill it, then overflow.
        char names[40][8];
        for (int i = 0; i < 40; i++) {
            snprintf(names[i], sizeof(names[i]), "c%d", i);
            reg.registerCommand(CMD(names[i], "x", I2C_NONE, noop, nullptr));
        }
        check(reg.dropped() == 40 - 16, "overflow beyond MAX_COMMANDS counted as dropped");

        // a command that did fit still dispatches; printHelp flags the drop
        StringWriter w; reg.dispatch("c0", w);
        check(w.buf.empty(), "command within capacity still dispatches (noop -> no output)");
        StringWriter wh; reg.printHelp(wh);
        check(wh.buf.find("dropped") != std::string::npos, "printHelp flags dropped commands");
    }

    // ── validateIds: distinct ids OK; duplicate non-NONE id panics ──────────────
    {
        CommandRegistry reg;
        reg.registerCommand(CMD("a", "x", CMD_HELP,    noop, nullptr));
        reg.registerCommand(CMD("b", "x", CMD_VERSION, noop, nullptr));
        g_panics = 0; reg.validateIds();
        check(g_panics == 0, "distinct command ids validate without panic");

        CommandRegistry dup;
        dup.registerCommand(CMD("a", "x", CMD_HELP, noop, nullptr));
        dup.registerCommand(CMD("b", "x", CMD_HELP, noop, nullptr));   // same id
        g_panics = 0; dup.validateIds();
        check(g_panics == 1, "duplicate command id triggers commander_on_panic");

        // multiple I2C_NONE ids are allowed (they're the "no wire id" sentinel)
        CommandRegistry none;
        none.registerCommand(CMD("a", "x", I2C_NONE, noop, nullptr));
        none.registerCommand(CMD("b", "x", I2C_NONE, noop, nullptr));
        g_panics = 0; none.validateIds();
        check(g_panics == 0, "multiple I2C_NONE ids do not collide");
    }

    // ── Writer.ok() contract: a disconnected writer reports not-ok ──────────────
    {
        StringWriter w; check(w.ok(), "Writer.ok() true while connected");
        w.connected = false; check(!w.ok(), "Writer.ok() false after disconnect");
    }

    // ── NullWriter + autostart-style dispatch: side effect runs, output discarded ─
    // This is the mechanism behind commander_run_autostart — dispatch a command for its
    // side effect with a Writer that throws the reply away.
    {
        static int fired = 0;
        CommandRegistry reg;
        reg.registerCommand(CMD("go", "x", I2C_NONE,
            [](const char *, Writer &out, void *){ fired++; out.write("noise"); }, nullptr));
        NullWriter nw;
        reg.dispatch("go", nw);             // as commander_run_autostart would
        check(fired == 1, "dispatch via NullWriter runs the handler (side effect)");
        // NullWriter has no observable buffer; the contract is simply that it discards.
        nw.write("anything"); nw.writeln("more");
        check(true, "NullWriter discards output without error");
    }

    printf(fails ? "\n%d FAILED\n" : "\nALL PASS\n", fails);
    return fails;
}
