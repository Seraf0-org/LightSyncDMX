// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncDMXModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FLightSyncDMXModule"

DEFINE_LOG_CATEGORY(LogLightSyncDMX);

void FLightSyncDMXModule::StartupModule()
{
    // Shaders/ ディレクトリを仮想パス "/LightSyncDMX" にマップする。
    // これにより IMPLEMENT_GLOBAL_SHADER の第2引数
    // "/LightSyncDMX/Private/LightSyncAverageCS.usf" が解決される。
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LightSyncDMX"));
    if (Plugin.IsValid())
    {
        FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
        AddShaderSourceDirectoryMapping(TEXT("/LightSyncDMX"), ShaderDir);

        UE_LOG(LogLightSyncDMX, Log,
               TEXT("LightSyncDMX module started. Shader dir: %s"), *ShaderDir);
    }
    else
    {
        UE_LOG(LogLightSyncDMX, Warning,
               TEXT("LightSyncDMX: プラグインが見つかりません。シェーダーパスのマッピングをスキップします。"));
    }
}

void FLightSyncDMXModule::ShutdownModule()
{
    UE_LOG(LogLightSyncDMX, Log, TEXT("LightSyncDMX module shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLightSyncDMXModule, LightSyncDMX)
