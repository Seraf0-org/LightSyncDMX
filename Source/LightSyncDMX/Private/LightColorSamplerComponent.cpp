// Copyright UE-Comp. All Rights Reserved.

#include "LightColorSamplerComponent.h"
#include "LightSyncGPUSampler.h"
#include "LightSyncDMXModule.h"

#include "Engine/TextureRenderTargetCube.h"

ULightColorSamplerComponent::ULightColorSamplerComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // Tick不要、LightProbeActorから呼ばれる
}

ULightColorSamplerComponent::~ULightColorSamplerComponent()
{
}

void ULightColorSamplerComponent::BeginPlay()
{
    Super::BeginPlay();
    bIsFirstSample = true;
    
    InitializeGPUSampler();
}

void ULightColorSamplerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseGPUSampler();
    
    Super::EndPlay(EndPlayReason);
}

void ULightColorSamplerComponent::OnUnregister()
{
    ReleaseGPUSampler();

    Super::OnUnregister();
}

void ULightColorSamplerComponent::BeginDestroy()
{
    ReleaseGPUSampler();

    Super::BeginDestroy();
}

void ULightColorSamplerComponent::InitializeGPUSampler()
{
    if (!GPUSampler)
    {
        GPUSampler = MakeUnique<FLightSyncGPUSampler>();
    }
    
    GPUSampler->Initialize();
    
    UpdateSamplerParams();
}

void ULightColorSamplerComponent::ReleaseGPUSampler()
{
    if (GPUSampler)
    {
        GPUSampler->Release();
        GPUSampler.Reset();
    }
}

void ULightColorSamplerComponent::UpdateSamplerParams()
{
    if (!GPUSampler)
    {
        return;
    }

    FLightSyncGPUSampler::FSamplingParams Params;
    Params.LuminanceExponent = LuminanceExponent;
    Params.DarkThreshold = DarkThreshold;
    Params.HDRClamp = HDRClampValue;
    Params.DirectLightThreshold = DirectLightThreshold;
    Params.bFilterDirectLight = bFilterDirectLight;
    Params.TopFaceWeight = TopFaceWeight;
    Params.BottomFaceWeight = BottomFaceWeight;
    Params.SideFaceWeight = SideFaceWeight;
    Params.ToneMappingExposure = ToneMappingExposure;
    Params.SaturationBoost = SaturationBoost;
    Params.DownsampleResolution = DownsampleResolution;
    GPUSampler->SetParams(Params);
    GPUSampler->SetForceCPUSync(SamplingMethod == ELightSyncSamplingMethod::CPUSync);
}

void ULightColorSamplerComponent::SampleFromRenderTarget(UTextureRenderTargetCube *InCubeRT)
{
    if (!bSamplingEnabled || !InCubeRT)
    {
        return;
    }

    FLinearColor NewAverageColor = FLinearColor::Black;
    FLinearColor NewTopColor = FLinearColor::Black;
    FLinearColor NewSideColor = FLinearColor::Black;
    FLinearColor NewDominantColor = FLinearColor::Black;

    if (!GPUSampler || !GPUSampler->IsInitialized())
    {
        InitializeGPUSampler();
    }

    if (GPUSampler)
    {
        UpdateSamplerParams();
        GPUSampler->DispatchSampling(InCubeRT);

        if (GPUSampler->TryGetResult(NewAverageColor, NewTopColor, NewSideColor, NewDominantColor))
        {
            RawSampledColor = NewAverageColor;
        }
        else
        {
            // GPU 結果がまだない場合は前の値を維持
            NewAverageColor = RawSampledColor;
            NewTopColor = TopColor;
            NewSideColor = SideColor;
            NewDominantColor = DominantColor;
        }
    }

    // スムージング適用
    if (bIsFirstSample)
    {
        SmoothedColor = NewAverageColor;
        TopColor = NewTopColor;
        SideColor = NewSideColor;
        DominantColor = NewDominantColor;
        bIsFirstSample = false;
    }
    else
    {
        SmoothedColor = SmoothColor(NewAverageColor, SmoothedColor, SmoothingAlpha);
        TopColor = SmoothColor(NewTopColor, PrevTopColor, SmoothingAlpha);
        SideColor = SmoothColor(NewSideColor, PrevSideColor, SmoothingAlpha);
        DominantColor = SmoothColor(NewDominantColor, PrevDominantColor, SmoothingAlpha);
    }

    // 前フレーム値を保存
    PrevTopColor = TopColor;
    PrevSideColor = SideColor;
    PrevDominantColor = DominantColor;
}

FLinearColor ULightColorSamplerComponent::SmoothColor(
    const FLinearColor &NewColor,
    const FLinearColor &PreviousColor,
    float Alpha)
{
    // EMA (指数移動平均) でちらつきを抑制
    FLinearColor Result;
    Result.R = FMath::Lerp(PreviousColor.R, NewColor.R, Alpha);
    Result.G = FMath::Lerp(PreviousColor.G, NewColor.G, Alpha);
    Result.B = FMath::Lerp(PreviousColor.B, NewColor.B, Alpha);
    Result.A = 1.0f;
    return Result;
}
