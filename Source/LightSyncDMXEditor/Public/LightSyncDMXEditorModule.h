// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FLightSyncDMXEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void RegisterMenuExtensions();
    void UnregisterMenuExtensions();

    TSharedPtr<class FUICommandList> PluginCommands;
    TSharedPtr<class SDockTab> LightSyncTab;

    FDelegateHandle LevelEditorTabManagerChangedHandle;
};
