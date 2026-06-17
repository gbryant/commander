#pragma once
#include "Writer.h"

// A Writer that discards everything written to it. Use when a command must be dispatched
// for its side effect but its output has nowhere (or no need) to go — e.g. the autostart
// boot commands (commander_run_autostart), which run before any session/transport is the
// obvious place to route to. Cheap: no buffer, no state.
class NullWriter : public Writer {
public:
    void write(const char *) override {}
    void writeln(const char * = "") override {}
};
