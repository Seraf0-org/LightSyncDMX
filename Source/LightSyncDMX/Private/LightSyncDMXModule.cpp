// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncDMXModule.h"

#define LOCTEXT_NAMESPACE "FLightSyncDMXModule"

DEFINE_LOG_CATEGORY(LogLightSyncDMX);

void FLightSyncDMXModule::StartupModule()
{
    UE_LOG(LogLightSyncDMX, Log, TEXT("LightSyncDMX module started."));
}

void FLightSyncDMXModule::ShutdownModule()
{
    UE_LOG(LogLightSyncDMX, Log, TEXT("LightSyncDMX module shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLightSyncDMXModule, LightSyncDMX)
