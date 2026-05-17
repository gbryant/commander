#pragma once

class CommandRegistry;

class IModule {
public:
    virtual ~IModule() = default;
    virtual const char *name()                           const = 0;
    virtual void        init()                                 = 0;
    virtual void        registerCommands(CommandRegistry &reg) = 0;
    virtual void        startTask()                            {}
    virtual void        tick()                                 {}
};
