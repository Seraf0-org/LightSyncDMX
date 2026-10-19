// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Containers/Ticker.h"

LIGHTSYNCDMX_API DECLARE_LOG_CATEGORY_EXTERN(LogLightSyncDMX, Log, All);

class FLightSyncDMXModule : public IModuleInterface
{
public:
    /** IModuleInterface implementation */
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    FDelegateHandle PreExitHandle;
    FTSTicker::FDelegateHandle ExitPollHandle;
};
