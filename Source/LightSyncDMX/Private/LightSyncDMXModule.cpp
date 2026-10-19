// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncDMXModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"
#include "LightColorSamplerComponent.h"
#include "GameFramework/Actor.h"

#define LOCTEXT_NAMESPACE "FLightSyncDMXModule"

DEFINE_LOG_CATEGORY(LogLightSyncDMX);

// 検証用: 全プローブで GPU 積分結果と CPU 版 (同じ式) を比べる
static FAutoConsoleCommand GLightSyncValidateGPUCommand(
    TEXT("LightSync.ValidateGPU"),
    TEXT("全 LightProbe の GPU ゾーン積分を CPU 版と比較してログに出す (静止シーンで使用)"),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        float WorstError = 0.0f;
        int32 Compared = 0;
        for (TObjectIterator<ULightColorSamplerComponent> It; It; ++It)
        {
            const ULightColorSamplerComponent *Sampler = *It;
            if (!IsValid(Sampler) || Sampler->IsTemplate() || !Sampler->GetOwner())
            {
                continue;
            }

            FString Report;
            float MaxError = 0.0f;
            if (Sampler->ValidateAgainstCPU(Report, MaxError))
            {
                ++Compared;
                WorstError = FMath::Max(WorstError, MaxError);
                UE_LOG(LogLightSyncDMX, Display, TEXT("ValidateGPU '%s': max error %.4f%%\n%s"),
                       *Sampler->GetOwner()->GetName(), MaxError * 100.0f, *Report);
            }
            else
            {
                UE_LOG(LogLightSyncDMX, Display, TEXT("ValidateGPU '%s': %s"), *Sampler->GetOwner()->GetName(), *Report);
            }
        }
        UE_LOG(LogLightSyncDMX, Display, TEXT("LIGHTSYNC_VALIDATE compared=%d maxerr=%.6f"), Compared, WorstError);
    }));

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
