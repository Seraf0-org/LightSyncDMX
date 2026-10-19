// Copyright UE-Comp. All Rights Reserved.

#include "LightProbeActor.h"
#include "LightColorSamplerComponent.h"
#include "DMXColorOutputComponent.h"
#include "OSCColorOutputComponent.h"
#include "LightSyncDMXModule.h"

#include "Components/SceneCaptureComponentCube.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTargetCube.h"
#include "UObject/ConstructorHelpers.h"
#include "Camera/CameraComponent.h"
#include "HAL/IConsoleManager.h"
#include "LightSyncNDisplay.h"
#include "LightSyncSubsystem.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Misc/App.h"

ALightProbeActor::ALightProbeActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostUpdateWork;

    // === Scene Root ===
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SetRootComponent(SceneRoot);

    // === Probe Mesh (視覚化用球体) ===
    ProbeMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProbeMesh"));
    ProbeMesh->SetupAttachment(SceneRoot);
    ProbeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ProbeMesh->SetCastShadow(false);
    ProbeMesh->SetVisibleInSceneCaptureOnly(false);
    ProbeMesh->SetHiddenInSceneCapture(true);
    ProbeMesh->SetHiddenInGame(true); // ゲーム中は非表示

    // 球体メッシュをデフォルト設定
    static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(
        TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    if (SphereMesh.Succeeded())
    {
        ProbeMesh->SetStaticMesh(SphereMesh.Object);
        ProbeMesh->SetWorldScale3D(FVector(0.3f)); // 小さめの球体
    }

    // === Scene Capture (360°キャプチャ) ===
    SceneCapture = CreateDefaultSubobject<USceneCaptureComponentCube>(TEXT("SceneCapture"));
    SceneCapture->SetupAttachment(SceneRoot);
    SceneCapture->bCaptureEveryFrame = false; // 手動で制御
    SceneCapture->bCaptureOnMovement = false;
    SceneCapture->bAlwaysPersistRenderingState = true;

    // CaptureSource / ShowFlags / PostProcess は基準モードに合わせて ApplyCaptureSettings() で毎回設定する
    SceneCapture->bCaptureRotation = false; // キューブの面は常にワールド軸 (ゾーン方向はプローブ回転で変換)

    // SceneCaptureは自身のProbeMeshをキャプチャしない
    SceneCapture->HiddenActors.Add(this);

    // === Color Sampler ===
    ColorSampler = CreateDefaultSubobject<ULightColorSamplerComponent>(TEXT("ColorSampler"));

    // === DMX Output ===
    DMXOutput = CreateDefaultSubobject<UDMXColorOutputComponent>(TEXT("DMXOutput"));

    // === OSC Output ===
    OSCOutput = CreateDefaultSubobject<UOSCColorOutputComponent>(TEXT("OSCOutput"));
}

bool ALightProbeActor::ShouldTickIfViewportsOnly() const
{
    return bIsProbeActive;
}

void ALightProbeActor::OnConstruction(const FTransform &Transform)
{
    Super::OnConstruction(Transform);

    // エディタに配置された時点でキャプチャを初期化
    EnsureCaptureInitialized();

    // エディタモードでもサブシステムに登録
    if (UWorld *World = GetWorld())
    {
        if (ULightSyncSubsystem *Subsystem = World->GetSubsystem<ULightSyncSubsystem>())
        {
            Subsystem->RegisterProbe(this);
        }
    }
}

void ALightProbeActor::EnsureCaptureInitialized()
{
    // CDO / コマンドレット / -nullrhi (クック・ヘッドレス実行) では GPU リソースを作らない
    if (IsTemplate() || !FApp::CanEverRender())
    {
        return;
    }

    if (!bCaptureInitialized)
    {
        InitializeCapture();
        bCaptureInitialized = (CaptureRenderTarget != nullptr && CaptureRenderTarget->GetResource() != nullptr);
    }
}

void ALightProbeActor::BeginPlay()
{
    Super::BeginPlay();

    EnsureCaptureInitialized();

    // サブシステムに自身を登録
    if (UWorld *World = GetWorld())
    {
        if (ULightSyncSubsystem *Subsystem = World->GetSubsystem<ULightSyncSubsystem>())
        {
            Subsystem->RegisterProbe(this);
        }
    }

    UE_LOG(LogLightSyncDMX, Log, TEXT("LightProbe '%s' started at location: %s"),
           *ProbeName, *GetActorLocation().ToString());
}

void ALightProbeActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // サブシステムから登録解除
    if (UWorld *World = GetWorld())
    {
        if (ULightSyncSubsystem *Subsystem = World->GetSubsystem<ULightSyncSubsystem>())
        {
            Subsystem->UnregisterProbe(this);
        }
    }

    Super::EndPlay(EndPlayReason);

    UE_LOG(LogLightSyncDMX, Log, TEXT("LightProbe '%s' stopped."), *ProbeName);
}

void ALightProbeActor::Destroyed()
{
    // エディタでの削除時は EndPlay が呼ばれないため、ここで消灯と登録解除を行う
    if (bBlackoutWhenInactive)
    {
        BlackoutOutputs();
    }

    if (UWorld *World = GetWorld())
    {
        if (ULightSyncSubsystem *Subsystem = World->GetSubsystem<ULightSyncSubsystem>())
        {
            Subsystem->UnregisterProbe(this);
        }
    }

    Super::Destroyed();
}

bool ALightProbeActor::ShouldSkipForPIE() const
{
    const UWorld *World = GetWorld();
    if (!World || World->WorldType != EWorldType::Editor || !GEngine)
    {
        return false;
    }

    // PIE 実行中はエディタワールド側のプローブを止め、PIE ワールドの複製だけが送信する
    for (const FWorldContext &Context : GEngine->GetWorldContexts())
    {
        if (Context.WorldType == EWorldType::PIE && Context.World() != nullptr)
        {
            return true;
        }
    }
    return false;
}

void ALightProbeActor::DrawDebugProbe() const
{
    UWorld *World = GetWorld();
    if (!World)
    {
        return;
    }

    const ULightSyncSubsystem *Subsystem = World->GetSubsystem<ULightSyncSubsystem>();
    if (!Subsystem || !Subsystem->bDrawDebug)
    {
        return;
    }

    const FVector Location = GetActorLocation();
    const FColor DrawColor = bIsProbeActive ? GetCurrentColor().ToFColor(true) : FColor(64, 64, 64);
    DrawDebugSphere(World, Location, 25.0f, 16, DrawColor, false, -1.0f, SDPG_Foreground, 2.0f);
    DrawDebugString(World, Location + FVector(0.0f, 0.0f, 40.0f),
                    FString::Printf(TEXT("%s (%d,%d,%d)"), *ProbeName, DrawColor.R, DrawColor.G, DrawColor.B),
                    nullptr, FColor::White, 0.0f, true);
}

void ALightProbeActor::BlackoutOutputs()
{
    if (bUseDMXOutput && DMXOutput)
    {
        DMXOutput->Blackout();
    }
    if (bUseOSCOutput && OSCOutput)
    {
        OSCOutput->Blackout();
    }
}

void ALightProbeActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (ShouldSkipForPIE())
    {
        return;
    }

    DrawDebugProbe();

    if (!bIsProbeActive)
    {
        return;
    }

    // サンプリングレートに基づく更新制御
    SamplingTimer += DeltaTime;
    const float SamplingInterval = 1.0f / FMath::Max(SamplingRate, 1.0f);

    if (SamplingTimer >= SamplingInterval)
    {
        SamplingTimer = FMath::Fmod(SamplingTimer, SamplingInterval);
        ForceSampleOnce();
    }
}

void ALightProbeActor::SetProbeActive(bool bActive)
{
    bIsProbeActive = bActive;

    // 無効化した瞬間に消灯 (最後の色のまま残らないように)
    if (!bActive && bBlackoutWhenInactive)
    {
        BlackoutOutputs();
    }

    if (SceneCapture)
    {
        SceneCapture->SetVisibility(bActive);
    }

    UE_LOG(LogLightSyncDMX, Verbose, TEXT("LightProbe '%s' %s"),
           *ProbeName, bActive ? TEXT("activated") : TEXT("deactivated"));
}

void ALightProbeActor::ForceSampleOnce()
{
    // エディタモードで呼ばれた場合に備えて遅延初期化
    EnsureCaptureInitialized();

    // Blueprint / モニターから CaptureResolution が直接変更された場合に追従
    if (CaptureRenderTarget && CaptureRenderTarget->SizeX != CaptureResolution)
    {
        UpdateRenderTarget();
    }

    if (!SceneCapture || !CaptureRenderTarget)
    {
        return;
    }

    // 1) 基準モードに合わせてキャプチャ設定を更新し、キュービックキャプチャを実行
    const FLightSyncSampleContext Context = ApplyCaptureSettings();
    SceneCapture->CaptureScene();

    // 2) ゾーンごとに積分 (GPU 結果は 1〜2 フレーム後に反映)
    if (ColorSampler)
    {
        ColorSampler->SampleFromRenderTarget(CaptureRenderTarget, Context);
    }

    // 3) 出力 (キャリブレーション中は止める)
    if (bSuspendOutput)
    {
        return;
    }
    if (bUseDMXOutput && DMXOutput && ColorSampler)
    {
        DMXOutput->SendZoneColors(ColorSampler->ZoneResults);
    }
    if (bUseOSCOutput && OSCOutput && ColorSampler)
    {
        OSCOutput->SendZoneColors(ColorSampler->ZoneResults);
    }
}

bool ALightProbeActor::GetZoneColor(FName ZoneName, FLinearColor &OutColor) const
{
    if (ColorSampler)
    {
        return ColorSampler->GetZoneColor(ZoneName, OutColor);
    }
    OutColor = FLinearColor::Black;
    return false;
}

FLinearColor ALightProbeActor::GetCurrentColor() const
{
    return (ColorSampler && ColorSampler->ZoneResults.Num() > 0) ? ColorSampler->ZoneResults[0].Color : FLinearColor::Black;
}

bool ALightProbeActor::GetReferenceCameraPostProcess(FPostProcessSettings &OutSettings) const
{
    if (!ReferenceCamera)
    {
        return false;
    }

    UCameraComponent *Camera = ReferenceCamera->FindComponentByClass<UCameraComponent>();
    if (!Camera)
    {
        return false;
    }

    // CineCamera の絞り等が反映された最終的なビュー設定を取る
    FMinimalViewInfo View;
    Camera->GetCameraView(0.0f, View);
    OutSettings = View.PostProcessSettings;
    return true;
}

namespace
{
    /**
     * UE の手動露出と同じ式で「シーン値 → カメラで 1.0 になる値」の倍率を求める。
     * PostProcessEyeAdaptation.cpp: ExposureScale = 0.18 / (LuminanceMax * 2^EV100) * 2^Bias
     */
    float ComputeManualExposureScale(float EV100, float Bias)
    {
        float LuminanceMax = 1.0f;
        const IConsoleVariable *ExtendRange = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DefaultFeature.AutoExposure.ExtendDefaultLuminanceRange"));
        const IConsoleVariable *LensAttenuation = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptation.LensAttenuation"));
        if (ExtendRange && ExtendRange->GetInt() != 0 && LensAttenuation)
        {
            LuminanceMax = 0.78f / FMath::Max(LensAttenuation->GetFloat(), 0.01f);
        }
        const float WhitePointLuminance = LuminanceMax * FMath::Pow(2.0f, EV100);
        return 0.18f / FMath::Max(WhitePointLuminance, 1e-6f) * FMath::Pow(2.0f, Bias);
    }

    /** 物理カメラ設定からの EV100 (CalculateManualAutoExposure と同じ式) */
    float PhysicalCameraEV100(const FPostProcessSettings &Settings)
    {
        if (!Settings.AutoExposureApplyPhysicalCameraExposure)
        {
            return 0.0f;
        }
        return FMath::Log2(FMath::Square(Settings.DepthOfFieldFstop) * Settings.CameraShutterSpeed * 100.0f /
                           FMath::Max(1.0f, Settings.CameraISO));
    }
}

FPostProcessSettings ALightProbeActor::BuildReferencePostProcess()
{
    FPostProcessSettings CameraPP;
    const bool bHasCamera = GetReferenceCameraPostProcess(CameraPP);

    if (bHasCamera && CameraPP.AutoExposureMethod != EAutoExposureMethod::AEM_Manual && !bWarnedAutoExposure)
    {
        UE_LOG(LogLightSyncDMX, Warning,
               TEXT("LightProbe '%s': 参照カメラの露出が Manual ではありません。プローブは Manual として計算します (ICVFX では Manual 露出を推奨)。"),
               *ProbeName);
        bWarnedAutoExposure = true;
    }

    // シーン側: 参照カメラ、なければ手動露出を物理カメラ (ISO 100 / 1 秒 / F=√2^EV) で表す。
    // nDisplay の露出補正が Bias だけを置き換えても EV100 が残るように、Bias に畳み込まない
    FPostProcessSettings ScenePP = CameraPP;
    if (!bHasCamera)
    {
        ScenePP = FPostProcessSettings();
        ScenePP.bOverride_AutoExposureMethod = true;
        ScenePP.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        ScenePP.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
        ScenePP.AutoExposureApplyPhysicalCameraExposure = true;
        ScenePP.bOverride_CameraISO = true;
        ScenePP.CameraISO = 100.0f;
        ScenePP.bOverride_CameraShutterSpeed = true;
        ScenePP.CameraShutterSpeed = 1.0f;
        ScenePP.bOverride_DepthOfFieldFstop = true;
        ScenePP.DepthOfFieldFstop = FMath::Sqrt(FMath::Pow(2.0f, ManualEV100));
        ScenePP.bOverride_AutoExposureBias = true;
        ScenePP.AutoExposureBias = ManualExposureBias;
    }

    if (PostProcessSource != ELightSyncPostProcessSource::NDisplay)
    {
        NDisplayReport.Reset();
        return ScenePP;
    }

    AActor *Root = NDisplayRootActor ? NDisplayRootActor.Get() : LightSyncNDisplay::FindRootActor(GetWorld());
    FPostProcessSettings Result;
    if (!LightSyncNDisplay::BuildOuterViewportPostProcess(Root, NDisplayViewportId, ScenePP, Result, NDisplayReport))
    {
        return ScenePP;
    }
    return Result;
}

FLightSyncSampleContext ALightProbeActor::ApplyCaptureSettings()
{
    FLightSyncSampleContext Context;
    Context.ProbeRotation = GetActorQuat();

    if (!SceneCapture)
    {
        return Context;
    }

    FEngineShowFlags &Flags = SceneCapture->ShowFlags;

    // 色ではなく「像」に効くエフェクトは常に無効 (照明の色・強さとは無関係で、積分値を歪める)
    Flags.SetAntiAliasing(false);
    Flags.SetTemporalAA(false);
    Flags.SetMotionBlur(false);
    Flags.SetDepthOfField(false);
    Flags.SetBloom(false);
    Flags.SetLensFlares(false);
    Flags.SetVignette(false);
    Flags.SetGrain(false);

    // 光の見え方に効くものはメインビューと同じく有効
    Flags.SetFog(true);
    Flags.SetVolumetricFog(true);
    Flags.SetAmbientOcclusion(true);

    const FPostProcessSettings ReferencePP = BuildReferencePostProcess();
    const float EV100 = PhysicalCameraEV100(ReferencePP);
    const float Bias = ReferencePP.AutoExposureBias;
    EffectiveExposureScale = ComputeManualExposureScale(EV100, Bias);

    if (ReferenceMode == ELightSyncReferenceMode::Wall)
    {
        // 露出・グレーディング・トーンカーブ適用後の値 (sRGB 原色)。
        // 実測で、この値は nDisplay が壁へ送る信号 (LDR のコード値 / 255) と一致する = 表示用にエンコード済み。
        // 壁が出す光は EOTF(信号) なので、texel ごとに WallDisplayEOTF でデコードしてから積分する
        SceneCapture->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
        Flags.SetPostProcessing(true);
        Flags.SetTonemapper(true);
        Flags.SetColorGrading(true);
        Flags.SetEyeAdaptation(true);

        FPostProcessSettings PP = ReferencePP;

        // 露出は必ず手動に固定 (キューブキャプチャが自分の中身に自動露出しないように)。
        // 物理カメラ分は Bias に畳み込む: 0.18 * 2^(Bias - EV100) と同じ倍率になる
        PP.bOverride_AutoExposureMethod = true;
        PP.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
        PP.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
        PP.AutoExposureApplyPhysicalCameraExposure = false;
        PP.bOverride_AutoExposureBias = true;
        PP.AutoExposureBias = Bias - EV100;

        SceneCapture->PostProcessSettings = PP;
        SceneCapture->PostProcessBlendWeight = 1.0f;

        Context.ExposureScale = OutputGain; // 露出はエンジンが適用済み
        Context.bInputIsSRGBGamut = true;
        Context.InputTransfer = WallDisplayEOTF;
    }
    else
    {
        // トーンマップ前の放射輝度 (作業色空間)。露出はプローブ側で掛ける
        SceneCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
        Flags.SetEyeAdaptation(false);
        SceneCapture->PostProcessSettings = FPostProcessSettings();
        SceneCapture->PostProcessBlendWeight = 0.0f;

        Context.ExposureScale = EffectiveExposureScale * OutputGain;
        Context.bInputIsSRGBGamut = false;
    }

    return Context;
}


void ALightProbeActor::InitializeCapture()
{
    UpdateRenderTarget();

    if (SceneCapture && CaptureRenderTarget)
    {
        SceneCapture->TextureTarget = CaptureRenderTarget;
    }
}

void ALightProbeActor::UpdateRenderTarget()
{
    if (IsTemplate() || !FApp::CanEverRender())
    {
        return;
    }

    // 既存のレンダーターゲットがあり、解像度が一致していればスキップ
    if (CaptureRenderTarget && CaptureRenderTarget->SizeX == CaptureResolution)
    {
        return;
    }

    // CubeMapレンダーターゲットを作成
    CaptureRenderTarget = NewObject<UTextureRenderTargetCube>(this, TEXT("CaptureRT"));
    if (!CaptureRenderTarget)
    {
        UE_LOG(LogLightSyncDMX, Error,
               TEXT("LightProbe '%s': レンダーターゲットの生成に失敗しました"), *ProbeName);
        return;
    }

    CaptureRenderTarget->Init(CaptureResolution, PF_FloatRGBA);
    CaptureRenderTarget->UpdateResourceImmediate(true);

    if (!CaptureRenderTarget->GetResource())
    {
        UE_LOG(LogLightSyncDMX, Error,
               TEXT("LightProbe '%s': レンダーターゲットのGPUリソース初期化に失敗しました"), *ProbeName);
        CaptureRenderTarget = nullptr;
        return;
    }

    if (SceneCapture)
    {
        SceneCapture->TextureTarget = CaptureRenderTarget;
    }

    UE_LOG(LogLightSyncDMX, Log, TEXT("LightProbe '%s': RenderTarget created (%dx%d)"),
           *ProbeName, CaptureResolution, CaptureResolution);
}



#if WITH_EDITOR
void ALightProbeActor::PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName PropertyName = PropertyChangedEvent.GetPropertyName();

    if (PropertyName == GET_MEMBER_NAME_CHECKED(ALightProbeActor, CaptureResolution))
    {
        UpdateRenderTarget();
    }
    else if (PropertyName == GET_MEMBER_NAME_CHECKED(ALightProbeActor, bIsProbeActive))
    {
        SetProbeActive(bIsProbeActive);
    }
}
#endif
