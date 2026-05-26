#pragma once

class Writer {
public:
    virtual ~Writer() = default;
    virtual void write(const char *s)        = 0;
    virtual void writeln(const char *s = "") = 0;
    // Returns false when the underlying transport has disconnected.
    // Streaming commands should poll this to exit cleanly.
    virtual bool ok() { return true; }
};
