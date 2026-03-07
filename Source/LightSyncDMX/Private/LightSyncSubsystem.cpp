// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncSubsystem.h"
#include "LightProbeActor.h"
#include "DMXColorOutputComponent.h"
#include "OSCColorOutputComponent.h"
#include "LightSyncDMXModule.h"

#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "EngineUtils.h"

void ULightSyncSubsystem::Initialize(FSubsystemCollectionBase &Collection)
{
    Super::Initialize(Collection);

    UE_LOG(LogLightSyncDMX, Log, TEXT("LightSyncSubsystem initialized."));
}

void ULightSyncSubsystem::Deinitialize()
{
    // 全プローブをブラックアウトしてからシャットダウン
    BlackoutAll();
    RegisteredProbes.Empty();

    UE_LOG(LogLightSyncDMX, Log, TEXT("LightSyncSubsystem deinitialized."));

    Super::Deinitialize();
}

bool ULightSyncSubsystem::ShouldCreateSubsystem(UObject *Outer) const
{
    // エディタ / ランタイム両方で有効
    return true;
}

void ULightSyncSubsystem::RegisterProbe(ALightProbeActor *Probe)
{
    if (!Probe)
    {
        return;
    }

    // 重複チェック
    for (const TWeakObjectPtr<ALightProbeActor> &Existing : RegisteredProbes)
    {
        if (Existing.Get() == Probe)
        {
            return;
        }
    }

    RegisteredProbes.Add(Probe);

    UE_LOG(LogLightSyncDMX, Log, TEXT("Probe registered: '%s' (Total: %d)"),
           *Probe->GetProbeName(), RegisteredProbes.Num());
}

void ULightSyncSubsystem::UnregisterProbe(ALightProbeActor *Probe)
{
    if (!Probe)
    {
        return;
    }

    RegisteredProbes.RemoveAll([Probe](const TWeakObjectPtr<ALightProbeActor> &Ptr)
                               { return Ptr.Get() == Probe; });

    UE_LOG(LogLightSyncDMX, Log, TEXT("Probe unregistered: '%s' (Remaining: %d)"),
           *Probe->GetProbeName(), RegisteredProbes.Num());
}

TArray<FLightSyncProbeInfo> ULightSyncSubsystem::GetAllProbeInfo() const
{
    TArray<FLightSyncProbeInfo> InfoArray;

    // RegisteredProbesが空の場合（エディタでBeginPlay前など）、ワールドから直接検索
    UWorld *World = GetWorld();
    if (RegisteredProbes.Num() == 0 && World)
    {
        for (TActorIterator<ALightProbeActor> It(World); It; ++It)
        {
            ALightProbeActor *Probe = *It;
            if (!Probe || !IsValid(Probe))
            {
                continue;
            }

            FLightSyncProbeInfo Info;
            Info.ProbeName = Probe->GetProbeName();
            Info.Location = Probe->GetActorLocation();
            Info.CurrentColor = Probe->GetCurrentColor();
            Info.bIsActive = Probe->bIsProbeActive;

            if (Probe->DMXOutput && Probe->DMXOutput->FixtureMappings.Num() > 0)
            {
                Info.Universe = Probe->DMXOutput->FixtureMappings[0].Universe;
                Info.StartChannel = Probe->DMXOutput->FixtureMappings[0].StartChannel;
            }

            InfoArray.Add(Info);
        }
        return InfoArray;
    }

    for (const TWeakObjectPtr<ALightProbeActor> &WeakProbe : RegisteredProbes)
    {
        ALightProbeActor *Probe = WeakProbe.Get();
        if (!Probe)
        {
            continue;
        }

        FLightSyncProbeInfo Info;
        Info.ProbeName = Probe->GetProbeName();
        Info.Location = Probe->GetActorLocation();
        Info.CurrentColor = Probe->GetCurrentColor();
        Info.bIsActive = Probe->bIsProbeActive;

        // DMXフィクスチャ情報の最初のエントリを取得
        if (Probe->DMXOutput && Probe->DMXOutput->FixtureMappings.Num() > 0)
        {
            Info.Universe = Probe->DMXOutput->FixtureMappings[0].Universe;
            Info.StartChannel = Probe->DMXOutput->FixtureMappings[0].StartChannel;
        }

        InfoArray.Add(Info);
    }

    return InfoArray;
}

ALightProbeActor *ULightSyncSubsystem::FindProbeByName(const FString &Name) const
{
    for (const TWeakObjectPtr<ALightProbeActor> &WeakProbe : RegisteredProbes)
    {
        ALightProbeActor *Probe = WeakProbe.Get();
        if (Probe && Probe->GetProbeName() == Name)
        {
            return Probe;
        }
    }

    return nullptr;
}

void ULightSyncSubsystem::SetAllProbesActive(bool bActive)
{
    CleanupInvalidProbes();

    for (const TWeakObjectPtr<ALightProbeActor> &WeakProbe : RegisteredProbes)
    {
        if (ALightProbeActor *Probe = WeakProbe.Get())
        {
            Probe->SetProbeActive(bActive);
        }
    }

    UE_LOG(LogLightSyncDMX, Log, TEXT("All probes %s (%d probes)"),
           bActive ? TEXT("activated") : TEXT("deactivated"), RegisteredProbes.Num());
}

void ULightSyncSubsystem::BlackoutAll()
{
    CleanupInvalidProbes();

    for (const TWeakObjectPtr<ALightProbeActor> &WeakProbe : RegisteredProbes)
    {
        if (ALightProbeActor *Probe = WeakProbe.Get())
        {
            if (Probe->DMXOutput)
            {
                Probe->DMXOutput->Blackout();
            }
            if (Probe->OSCOutput)
            {
                Probe->OSCOutput->Blackout();
            }
        }
    }

    UE_LOG(LogLightSyncDMX, Log, TEXT("All probes blacked out."));
}

void ULightSyncSubsystem::ForceSampleAll()
{
    CleanupInvalidProbes();

    for (const TWeakObjectPtr<ALightProbeActor> &WeakProbe : RegisteredProbes)
    {
        if (ALightProbeActor *Probe = WeakProbe.Get())
        {
            Probe->ForceSampleOnce();
        }
    }
}

void ULightSyncSubsystem::SetMasterDimmer(float Dimmer)
{
    CleanupInvalidProbes();

    const float ClampedDimmer = FMath::Clamp(Dimmer, 0.0f, 1.0f);

    for (const TWeakObjectPtr<ALightProbeActor> &WeakProbe : RegisteredProbes)
    {
        if (ALightProbeActor *Probe = WeakProbe.Get())
        {
            if (Probe->DMXOutput)
            {
                Probe->DMXOutput->MasterDimmer = ClampedDimmer;
            }
        }
    }
}

void ULightSyncSubsystem::DumpProbeStatus() const
{
    UE_LOG(LogLightSyncDMX, Log, TEXT("=== LightSync Probe Status ==="));
    UE_LOG(LogLightSyncDMX, Log, TEXT("Registered Probes: %d"), RegisteredProbes.Num());

    for (int32 i = 0; i < RegisteredProbes.Num(); ++i)
    {
        const ALightProbeActor *Probe = RegisteredProbes[i].Get();
        if (!Probe)
        {
            UE_LOG(LogLightSyncDMX, Log, TEXT("  [%d] <INVALID>"), i);
            continue;
        }

        const FLinearColor Color = Probe->GetCurrentColor();
        UE_LOG(LogLightSyncDMX, Log,
               TEXT("  [%d] '%s' Active=%s Pos=%s Color=(R=%.3f G=%.3f B=%.3f)"),
               i,
               *Probe->GetProbeName(),
               Probe->bIsProbeActive ? TEXT("YES") : TEXT("NO"),
               *Probe->GetActorLocation().ToString(),
               Color.R, Color.G, Color.B);
    }

    UE_LOG(LogLightSyncDMX, Log, TEXT("=============================="));
}

void ULightSyncSubsystem::CleanupInvalidProbes()
{
    RegisteredProbes.RemoveAll([](const TWeakObjectPtr<ALightProbeActor> &Ptr)
                               { return !Ptr.IsValid(); });
}
