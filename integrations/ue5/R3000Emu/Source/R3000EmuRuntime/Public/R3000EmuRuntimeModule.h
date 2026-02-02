#pragma once

#include "Modules/ModuleManager.h"

class FR3000EmuRuntimeModule final : public IModuleInterface
{
  public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};

