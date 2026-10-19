// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncDMXModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UObjectIterator.h"
#include "LightColorSamplerComponent.h"
#include "LightProbeActor.h"
#include "GameFramework/Actor.h"
#include "Misc/CoreDelegates.h"
#include "Engine/World.h"
#include "LightSyncSettings.h"
#include "LightSyncDirectOutput.h"

#define LOCTEXT_NAMESPACE "FLightSyncDMXModule"

DEFINE_LOG_CATEGORY(LogLightSyncDMX);

// 性能: 全プローブのキャプチャ数・省略数・遅延・ゲームスレッド時間
static FAutoConsoleCommand GLightSyncStatsCommand(
    TEXT("LightSync.Stats"),
    TEXT("全 LightProbe のキャプチャ数 / 結果待ちで省いた数 / フレーム上限で持ち越した数 / 結果の遅延 / ゲームスレッド時間を表示"),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        for (TObjectIterator<ALightProbeActor> It; It; ++It)
        {
            const ALightProbeActor *Probe = *It;
            if (!IsValid(Probe) || Probe->IsTemplate() || !Probe->GetWorld())
            {
                continue;
            }
            FString Reason;
            const bool bOutput = LightSyncOutput::CanOutput(Probe->GetWorld(), &Reason);
            UE_LOG(LogLightSyncDMX, Display,
                   TEXT("LIGHTSYNC_STATS probe='%s' world=%s captures=%d skipped_busy=%d deferred_budget=%d latency_ms=%.1f gt_ms=%.3f idle=%s idle_s=%.1f output=%s (%s)"),
                   *Probe->ProbeName, *Probe->GetWorld()->GetName(), Probe->CapturesPerformed, Probe->CapturesSkippedBusy,
                   Probe->CapturesDeferredBudget, Probe->ColorSampler ? Probe->ColorSampler->LastResultLatencyMs : 0.0f,
                   Probe->AvgCaptureGameThreadMs, Probe->bIsIdle ? TEXT("yes") : TEXT("no"), Probe->IdleSeconds,
                   bOutput ? TEXT("yes") : TEXT("no"), *Reason);
        }
    }));

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
    // 終了処理では UE の DMX ポートが送信を止めるので、自前で直接ブラックアウトを送る。
    // 1) 終了が要求された直後のフレーム: コンソールウィンドウを閉じた場合などは、終了処理の途中で
    //    OS にプロセスを止められる (約 5 秒) ので、OnPreExit を待たずにすぐ送る
    ExitPollHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
    {
        if (!IsEngineExitRequested())
        {
            return true;
        }
        if (GetDefault<ULightSyncSettings>()->bDirectBlackoutOnExit)
        {
            LightSyncDirectOutput::SendBlackoutNow();
        }
        return false; // 一度だけ
    }));

    // 2) 終了処理の最後にもう一度 (1 の後に出た値も消す)
    PreExitHandle = FCoreDelegates::OnPreExit.AddLambda([]()
    {
        if (GetDefault<ULightSyncSettings>()->bDirectBlackoutOnExit)
        {
            LightSyncDirectOutput::SendBlackoutNow();
        }
        LightSyncDirectOutput::Reset();
    });

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
    FCoreDelegates::OnPreExit.Remove(PreExitHandle);
    FTSTicker::RemoveTicker(ExitPollHandle);

    UE_LOG(LogLightSyncDMX, Log, TEXT("LightSyncDMX module shutdown."));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLightSyncDMXModule, LightSyncDMX)
