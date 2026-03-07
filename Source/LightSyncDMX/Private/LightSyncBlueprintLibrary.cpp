// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncBlueprintLibrary.h"
#include "LightProbeActor.h"
#include "LightSyncSubsystem.h"

#include "Engine/World.h"
#include "EngineUtils.h"

TArray<ALightProbeActor *> ULightSyncBlueprintLibrary::GetAllLightProbes(const UObject *WorldContextObject)
{
    TArray<ALightProbeActor *> Result;

    if (!WorldContextObject)
    {
        return Result;
    }

    UWorld *World = WorldContextObject->GetWorld();
    if (!World)
    {
        return Result;
    }

    for (TActorIterator<ALightProbeActor> It(World); It; ++It)
    {
        Result.Add(*It);
    }

    return Result;
}

void ULightSyncBlueprintLibrary::SetAllProbesActive(const UObject *WorldContextObject, bool bActive)
{
    if (!WorldContextObject)
    {
        return;
    }

    UWorld *World = WorldContextObject->GetWorld();
    if (!World)
    {
        return;
    }

    ULightSyncSubsystem *Subsystem = World->GetSubsystem<ULightSyncSubsystem>();
    if (Subsystem)
    {
        Subsystem->SetAllProbesActive(bActive);
    }
}

void ULightSyncBlueprintLibrary::BlackoutAllProbes(const UObject *WorldContextObject)
{
    if (!WorldContextObject)
    {
        return;
    }

    UWorld *World = WorldContextObject->GetWorld();
    if (!World)
    {
        return;
    }

    ULightSyncSubsystem *Subsystem = World->GetSubsystem<ULightSyncSubsystem>();
    if (Subsystem)
    {
        Subsystem->BlackoutAll();
    }
}

void ULightSyncBlueprintLibrary::SetMasterDimmer(const UObject *WorldContextObject, float Dimmer)
{
    if (!WorldContextObject)
    {
        return;
    }

    UWorld *World = WorldContextObject->GetWorld();
    if (!World)
    {
        return;
    }

    ULightSyncSubsystem *Subsystem = World->GetSubsystem<ULightSyncSubsystem>();
    if (Subsystem)
    {
        Subsystem->SetMasterDimmer(Dimmer);
    }
}

void ULightSyncBlueprintLibrary::LinearColorToDMXValues(
    const FLinearColor &Color, int32 &OutR, int32 &OutG, int32 &OutB)
{
    OutR = FMath::Clamp(FMath::RoundToInt(Color.R * 255.0f), 0, 255);
    OutG = FMath::Clamp(FMath::RoundToInt(Color.G * 255.0f), 0, 255);
    OutB = FMath::Clamp(FMath::RoundToInt(Color.B * 255.0f), 0, 255);
}

FLinearColor ULightSyncBlueprintLibrary::DMXValuesToLinearColor(int32 R, int32 G, int32 B)
{
    return FLinearColor(
        FMath::Clamp(R, 0, 255) / 255.0f,
        FMath::Clamp(G, 0, 255) / 255.0f,
        FMath::Clamp(B, 0, 255) / 255.0f,
        1.0f);
}
