#pragma once

class Writer {
public:
    virtual ~Writer() = default;
    virtual void write(const char *s)        = 0;
    virtual void writeln(const char *s = "") = 0;
};
