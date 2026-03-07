// Copyright UE-Comp. All Rights Reserved.

#include "LightProbeActor.h"
#include "LightColorSamplerComponent.h"
#include "DMXColorOutputComponent.h"
#include "OSCColorOutputComponent.h"
#include "LightSyncDMXModule.h"

#include "Components/SceneCaptureComponentCube.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/TextureRenderTargetCube.h"
#include "UObject/ConstructorHelpers.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "LightSyncSubsystem.h"

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
    ProbeMesh->bVisibleInSceneCaptureOnly = false;
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

    // === 正確な色キャプチャのための設定 ===
    // パフォーマンス最適化
    SceneCapture->ShowFlags.SetAntiAliasing(false);
    SceneCapture->ShowFlags.SetMotionBlur(false);

    // 色に影響するポストプロセスを全て無効化 (生リニアHDR取得)
    SceneCapture->ShowFlags.SetTonemapper(false);        // ★ ACES等のトーンマッピングを無効化
    SceneCapture->ShowFlags.SetEyeAdaptation(false);     // 自動露出を無効化
    SceneCapture->ShowFlags.SetBloom(false);              // ブルームを無効化
    SceneCapture->ShowFlags.SetColorGrading(false);       // カラーグレーディングを無効化
    SceneCapture->ShowFlags.SetVignette(false);           // ビネットを無効化
    SceneCapture->ShowFlags.SetGrain(false);              // フィルムグレインを無効化
    SceneCapture->ShowFlags.SetLensFlares(false);         // レンズフレアを無効化
    SceneCapture->ShowFlags.SetAmbientOcclusion(false);   // AO (色に影響するため)

    // 環境エフェクト (不要)
    SceneCapture->ShowFlags.SetFog(false);
    SceneCapture->ShowFlags.SetVolumetricFog(false);

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
    if (!bCaptureInitialized)
    {
        InitializeCapture();
        bCaptureInitialized = (CaptureRenderTarget != nullptr);
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

void ALightProbeActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (!bIsProbeActive)
    {
        return;
    }

    // サンプリングレートに基づく更新制御
    SamplingTimer += DeltaTime;
    const float SamplingInterval = 1.0f / FMath::Max(SamplingRate, 1.0f);

    if (SamplingTimer >= SamplingInterval)
    {
        SamplingTimer = 0.0f;
        ForceSampleOnce();
    }
}

void ALightProbeActor::SetProbeActive(bool bActive)
{
    bIsProbeActive = bActive;

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

    if (!SceneCapture || !CaptureRenderTarget)
    {
        return;
    }

    // 1) SceneCaptureでキュービックキャプチャを実行
    SceneCapture->CaptureScene();

    // 2) ColorSamplerでレンダーターゲットから平均色を読み取り
    if (ColorSampler)
    {
        ColorSampler->SampleFromRenderTarget(CaptureRenderTarget);
        CurrentSampledColor = ColorSampler->GetSampledColor();
    }

    // 3) 色補正を適用
    CorrectedOutputColor = ApplyColorCorrection(CurrentSampledColor);

    // 4) 色のみモード: 明度を正規化して色相・彩度のみ送信
    FLinearColor OutputColor = CorrectedOutputColor;
    if (bColorOnly)
    {
        const float MaxComp = FMath::Max3(OutputColor.R, OutputColor.G, OutputColor.B);
        if (MaxComp > ColorOnlyBlackThreshold)
        {
            // max(R,G,B) = 1.0 に正規化 → 純粋な色だけ
            OutputColor.R /= MaxComp;
            OutputColor.G /= MaxComp;
            OutputColor.B /= MaxComp;
        }
        else
        {
            // 暗すぎる場合はブラックを送信
            OutputColor = FLinearColor::Black;
        }
    }

    // 5) DMX出力に色を渡す
    if (bUseDMXOutput && DMXOutput)
    {
        DMXOutput->SendColor(OutputColor);
    }

    // 6) OSC出力に色を渡す (別PCへのネットワーク送信)
    if (bUseOSCOutput && OSCOutput)
    {
        OSCOutput->SendColor(OutputColor);
    }
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
    // 既存のレンダーターゲットがあり、解像度が一致していればスキップ
    if (CaptureRenderTarget && CaptureRenderTarget->SizeX == CaptureResolution)
    {
        return;
    }

    // CubeMapレンダーターゲットを作成
    CaptureRenderTarget = NewObject<UTextureRenderTargetCube>(this, TEXT("CaptureRT"));
    CaptureRenderTarget->Init(CaptureResolution, PF_FloatRGBA);
    CaptureRenderTarget->UpdateResourceImmediate(true);

    if (SceneCapture)
    {
        SceneCapture->TextureTarget = CaptureRenderTarget;
    }

    UE_LOG(LogLightSyncDMX, Log, TEXT("LightProbe '%s': RenderTarget created (%dx%d)"),
           *ProbeName, CaptureResolution, CaptureResolution);
}

FLinearColor ALightProbeActor::ApplyColorCorrection(const FLinearColor &RawColor) const
{
    FLinearColor Result = RawColor;

    // --- ガンマ補正 ---
    if (!FMath::IsNearlyEqual(GammaCorrection, 1.0f))
    {
        const float InvGamma = 1.0f / FMath::Max(GammaCorrection, 0.01f);
        Result.R = FMath::Pow(FMath::Max(Result.R, 0.0f), InvGamma);
        Result.G = FMath::Pow(FMath::Max(Result.G, 0.0f), InvGamma);
        Result.B = FMath::Pow(FMath::Max(Result.B, 0.0f), InvGamma);
    }

    // --- 色温度オフセット ---
    if (!FMath::IsNearlyZero(ColorTemperatureOffset))
    {
        // 基準6500K + オフセット でRGB係数を得る
        const FLinearColor TempTint = ColorTemperatureToRGB(6500.0f + ColorTemperatureOffset);
        Result.R *= TempTint.R;
        Result.G *= TempTint.G;
        Result.B *= TempTint.B;
    }

    // --- 彩度調整 ---
    if (!FMath::IsNearlyEqual(SaturationMultiplier, 1.0f))
    {
        // 輝度計算 (Rec.709)
        const float Luminance = 0.2126f * Result.R + 0.7152f * Result.G + 0.0722f * Result.B;
        Result.R = FMath::Lerp(Luminance, Result.R, SaturationMultiplier);
        Result.G = FMath::Lerp(Luminance, Result.G, SaturationMultiplier);
        Result.B = FMath::Lerp(Luminance, Result.B, SaturationMultiplier);
    }

    // --- 明度調整 ---
    Result.R *= BrightnessMultiplier;
    Result.G *= BrightnessMultiplier;
    Result.B *= BrightnessMultiplier;

    // クランプ
    Result.R = FMath::Clamp(Result.R, 0.0f, 1.0f);
    Result.G = FMath::Clamp(Result.G, 0.0f, 1.0f);
    Result.B = FMath::Clamp(Result.B, 0.0f, 1.0f);
    Result.A = 1.0f;

    return Result;
}

FLinearColor ALightProbeActor::ColorTemperatureToRGB(float TempKelvin)
{
    // Tanner Helland's algorithm を使用した色温度→RGB変換
    // http://www.tannerhelland.com/4435/convert-temperature-rgb-algorithm-code/

    TempKelvin = FMath::Clamp(TempKelvin, 1000.0f, 40000.0f) / 100.0f;

    float R, G, B;

    // Red
    if (TempKelvin <= 66.0f)
    {
        R = 1.0f;
    }
    else
    {
        R = TempKelvin - 60.0f;
        R = 329.698727446f * FMath::Pow(R, -0.1332047592f);
        R = FMath::Clamp(R / 255.0f, 0.0f, 1.0f);
    }

    // Green
    if (TempKelvin <= 66.0f)
    {
        G = TempKelvin;
        G = 99.4708025861f * FMath::Loge(G) - 161.1195681661f;
        G = FMath::Clamp(G / 255.0f, 0.0f, 1.0f);
    }
    else
    {
        G = TempKelvin - 60.0f;
        G = 288.1221695283f * FMath::Pow(G, -0.0755148492f);
        G = FMath::Clamp(G / 255.0f, 0.0f, 1.0f);
    }

    // Blue
    if (TempKelvin >= 66.0f)
    {
        B = 1.0f;
    }
    else if (TempKelvin <= 19.0f)
    {
        B = 0.0f;
    }
    else
    {
        B = TempKelvin - 10.0f;
        B = 138.5177312231f * FMath::Loge(B) - 305.0447927307f;
        B = FMath::Clamp(B / 255.0f, 0.0f, 1.0f);
    }

    return FLinearColor(R, G, B, 1.0f);
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
