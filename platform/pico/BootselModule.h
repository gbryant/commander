#pragma once
#include "core/IModule.h"

class BootselModule : public IModule {
public:
    // Call before stdio_init_all() in main(). If the bootloader command
    // triggered the reboot, this enters USB bootloader immediately.
    static void checkAtBoot();

    const char *name()  const override { return "bootsel"; }
    void        init()        override {}
    void        registerCommands(CommandRegistry &reg) override;
    void        startTask()   override {}
};
