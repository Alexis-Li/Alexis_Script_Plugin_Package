#pragma once

#include "Modules/ModuleManager.h"

class F{{PLUGIN_NAME}}EditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
};
