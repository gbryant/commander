#pragma once
#include "IModule.h"
#include "CommandRegistry.h"

class SystemModule : public IModule {
public:
    const char *name()  const override { return "system"; }
    void        init()        override {}
    void        registerCommands(CommandRegistry &reg) override;

private:
    CommandRegistry *_reg = nullptr;
};

inline void SystemModule::registerCommands(CommandRegistry &reg) {
    _reg = &reg;
    reg.registerCommand(CMD("help", "list all commands", CMD_HELP,
        [](const char *, Writer &out, void *ctx) {
            static_cast<CommandRegistry *>(ctx)->printHelp(out);
        }, _reg));
}
