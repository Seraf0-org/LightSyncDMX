// Copyright UE-Comp. All Rights Reserved.

#include "LightColorSamplerComponent.h"
#include "LightSyncGPUSampler.h"
#include "LightSyncDMXModule.h"

#include "ColorManagement/ColorSpace.h"
#include "Engine/TextureRenderTargetCube.h"
#include "HAL/PlatformTime.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

ULightColorSamplerComponent::ULightColorSamplerComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // Tick不要、LightProbeActorから呼ばれる
    Zones = LightSyncZones::MakeDefaultZones();
}

void ULightColorSamplerComponent::BeginPlay()
{
    Super::BeginPlay();
    LastResultTime = -1.0;
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
}

void ULightColorSamplerComponent::ReleaseGPUSampler()
{
    if (GPUSampler)
    {
        GPUSampler->Release();
        GPUSampler.Reset();
    }
}

TArray<FLightSyncZone> ULightColorSamplerComponent::GetActiveZones() const
{
    TArray<FLightSyncZone> Active;
    for (const FLightSyncZone &Zone : Zones)
    {
        if (Zone.bEnabled && Active.Num() < LightSyncZones::MaxZones)
        {
            Active.Add(Zone);
        }
    }
    return Active;
}

void ULightColorSamplerComponent::ResetZonesToDefault()
{
    Zones = LightSyncZones::MakeDefaultZones();
    LastZoneNames.Reset();
}

bool ULightColorSamplerComponent::CanDispatch() const
{
    return bSamplingEnabled && !(GPUSampler && GPUSampler->IsBusy());
}

void ULightColorSamplerComponent::Dispatch(UTextureRenderTargetCube *InCubeRT, const FLightSyncSampleContext &Context)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(LightSync_SamplerDispatch);

    if (!bSamplingEnabled || !InCubeRT)
    {
        return;
    }

    if (!GPUSampler || !GPUSampler->IsInitialized())
    {
        InitializeGPUSampler();
    }

    // ピーク探索に使う輝度係数は、キャプチャ値の色空間に合わせる
    const FLinearColor LumFactors = Context.bInputIsSRGBGamut
        ? FLinearColor(0.2126f, 0.7152f, 0.0722f)
        : UE::Color::FColorSpace::GetWorking().GetLuminanceFactors();

    const TArray<FLightSyncZone> Active = GetActiveZones();
    LightSyncZones::FSetup Setup = LightSyncZones::BuildSetup(Active, Context.ProbeRotation, PartitionSharpness,
                                                              FVector3f(LumFactors.R, LumFactors.G, LumFactors.B),
                                                              Context.InputTransfer);
    Setup.TransferGamma = Context.TransferGamma;

    GPUSampler->SetSetup(Setup);
    GPUSampler->SetForceCPUSync(SamplingMethod == ELightSyncSamplingMethod::CPUSync);
    GPUSampler->DispatchSampling(InCubeRT);

    PendingContext = Context;
    DispatchTime = FPlatformTime::Seconds();
    LastSetup = Setup;
    LastRenderTarget = InCubeRT;
}

bool ULightColorSamplerComponent::PollNewResult()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(LightSync_SamplerPoll);

    if (!GPUSampler)
    {
        return false;
    }

    GPUSampler->Poll();

    LightSyncZones::FResult Result;
    if (!GPUSampler->TryGetNewResult(Result))
    {
        return false;
    }

    LastResultLatencyMs = static_cast<float>((FPlatformTime::Seconds() - DispatchTime) * 1000.0);
    LastRawResult = Result;
    ApplyNewResult(Result, PendingContext);
    return true;
}

void ULightColorSamplerComponent::SampleFromRenderTarget(UTextureRenderTargetCube *InCubeRT, const FLightSyncSampleContext &Context)
{
    if (CanDispatch())
    {
        Dispatch(InCubeRT, Context);
    }
    PollNewResult();
}

void ULightColorSamplerComponent::ApplyNewResult(const LightSyncZones::FResult &Result, const FLightSyncSampleContext &Context)
{
    const TArray<FLightSyncZone> Active = GetActiveZones();

    // 設定変更直後はディスパッチ時と現在のゾーン数が合わないことがある → 捨てて次を待つ
    if (Result.Zones.Num() != Active.Num())
    {
        return;
    }

    TArray<FName> Names;
    for (const FLightSyncZone &Zone : Active)
    {
        Names.Add(Zone.Name);
    }

    const double Now = FPlatformTime::Seconds();
    const bool bReset = (Names != LastZoneNames) || LastResultTime < 0.0 || ZoneResults.Num() != Active.Num();
    float Alpha = 1.0f;
    if (!bReset && SmoothingTimeConstant > 0.0f)
    {
        const double Dt = FMath::Max(Now - LastResultTime, 0.0);
        Alpha = static_cast<float>(1.0 - FMath::Exp(-Dt / SmoothingTimeConstant));
    }

    if (bReset)
    {
        ZoneResults.SetNum(Active.Num());
    }
    LastTargetColors.SetNum(Active.Num());

    for (int32 Index = 0; Index < Active.Num(); ++Index)
    {
        const FVector4f &Raw = Result.Zones[Index];
        FLinearColor Color(Raw.X, Raw.Y, Raw.Z, 1.0f);

        // sRGB 原色のキャプチャ (FinalToneCurveHDR) は作業色空間へ変換。線形変換なので積分後に掛けてよい
        if (Context.bInputIsSRGBGamut)
        {
            Color = UE::Color::FColorSpaceTransform::GetSRGBToWorkingColorSpace().Apply(Color);
        }
        Color *= Context.ExposureScale;
        Color.A = 1.0f;
        LastTargetColors[Index] = Color;

        FLightSyncZoneResult &Out = ZoneResults[Index];
        Out.Name = Active[Index].Name;
        Out.CoverageSr = Raw.W;
        Out.Color = bReset ? Color : FMath::Lerp(Out.Color, Color, Alpha);
        Out.Color.A = 1.0f;
    }

    KeyDirection = FVector(Result.KeyDirection);
    LastZoneNames = MoveTemp(Names);
    LastResultTime = Now;
}

bool ULightColorSamplerComponent::ValidateAgainstCPU(FString &OutReport, float &OutMaxRelError) const
{
    OutMaxRelError = 0.0f;
    UTextureRenderTargetCube *RT = LastRenderTarget.Get();
    if (!RT || LastRawResult.Zones.Num() != LastSetup.Zones.Num() || LastSetup.Zones.Num() == 0)
    {
        OutReport = TEXT("比較できる GPU 結果がまだありません");
        return false;
    }

    TArray<FLinearColor> Faces[6];
    int32 Size = 0;
    if (!FLightSyncGPUSampler::ReadCubeTexels(RT, Faces, Size))
    {
        OutReport = TEXT("キューブマップを CPU で読み出せませんでした");
        return false;
    }

    const LightSyncZones::FResult Cpu = LightSyncZones::IntegrateCube(Faces, Size, LastSetup);

    // 相対誤差。ほぼ 0 の値は絶対誤差で見る (暗いゾーンで誤差が膨らまないように)
    auto RelError = [](float Gpu, float Ref)
    {
        return FMath::Abs(Gpu - Ref) / FMath::Max(FMath::Abs(Ref), 1e-3f);
    };

    OutReport.Reset();
    for (int32 Index = 0; Index < Cpu.Zones.Num(); ++Index)
    {
        const FVector4f &G = LastRawResult.Zones[Index];
        const FVector4f &C = Cpu.Zones[Index];
        const float Err = FMath::Max(FMath::Max(RelError(G.X, C.X), RelError(G.Y, C.Y)),
                                     FMath::Max(RelError(G.Z, C.Z), RelError(G.W, C.W)));
        OutMaxRelError = FMath::Max(OutMaxRelError, Err);
        OutReport += FString::Printf(TEXT("  zone[%d] GPU=(%.5f %.5f %.5f | %.4f sr) CPU=(%.5f %.5f %.5f | %.4f sr) err=%.4f%%\n"),
                                     Index, G.X, G.Y, G.Z, G.W, C.X, C.Y, C.Z, C.W, Err * 100.0f);
    }

    const float KeyAngle = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
        FVector3f::DotProduct(LastRawResult.KeyDirection, Cpu.KeyDirection), -1.0f, 1.0f)));
    OutReport += FString::Printf(TEXT("  key direction difference = %.3f deg\n"), KeyAngle);
    return true;
}

bool ULightColorSamplerComponent::GetZoneColor(FName ZoneName, FLinearColor &OutColor) const
{
    for (const FLightSyncZoneResult &Result : ZoneResults)
    {
        if (Result.Name == ZoneName)
        {
            OutColor = Result.Color;
            return true;
        }
    }
    OutColor = FLinearColor::Black;
    return false;
}

#if WITH_EDITOR
void ULightColorSamplerComponent::PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    if (PropertyChangedEvent.GetMemberPropertyName() != GET_MEMBER_NAME_CHECKED(ULightColorSamplerComponent, Zones))
    {
        return;
    }

    int32 NumEnabled = 0;
    TSet<FName> SeenNames;
    for (const FLightSyncZone &Zone : Zones)
    {
        if (!Zone.bEnabled)
        {
            continue;
        }
        ++NumEnabled;
        if (SeenNames.Contains(Zone.Name))
        {
            UE_LOG(LogLightSyncDMX, Warning, TEXT("ゾーン名 '%s' が重複しています。出力側は最初のゾーンを使います。"),
                   *Zone.Name.ToString());
        }
        SeenNames.Add(Zone.Name);
    }

    if (NumEnabled > LightSyncZones::MaxZones)
    {
        UE_LOG(LogLightSyncDMX, Warning, TEXT("有効なゾーンが %d 個あります。先頭の %d 個だけが計算されます。"),
               NumEnabled, LightSyncZones::MaxZones);
    }
}
#endif
