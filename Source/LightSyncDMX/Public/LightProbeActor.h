// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "LightProbeActor.generated.h"

class ULightColorSamplerComponent;
class UDMXColorOutputComponent;
class UOSCColorOutputComponent;
class USceneCaptureComponentCube;
class UTextureRenderTargetCube;
class UStaticMeshComponent;
class UBillboardComponent;

/**
 * ALightProbeActor
 *
 * シーンに配置する光サンプリングプローブ。
 * 配置位置の周囲360°の光を SceneCaptureComponentCube でキャプチャし、
 * 平均色を算出してDMX信号として出力する。
 *
 * VP(バーチャルプロダクション)でLEDウォール/照明の色味を
 * UEのシーンと一致させるために使用する。
 */
UCLASS(BlueprintType, Blueprintable, meta = (DisplayName = "Light Probe (DMX)"))
class LIGHTSYNCDMX_API ALightProbeActor : public AActor
{
    GENERATED_BODY()

public:
    ALightProbeActor();

    // === コンポーネント ===

    /** シーンルート */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LightProbe")
    TObjectPtr<USceneComponent> SceneRoot;

    /** プローブの視覚的表示用メッシュ (エディタ/デバッグ用) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LightProbe|Visualization")
    TObjectPtr<UStaticMeshComponent> ProbeMesh;

    /** 360°シーンキャプチャ */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LightProbe|Capture")
    TObjectPtr<USceneCaptureComponentCube> SceneCapture;

    /** 光の色をサンプリングするコンポーネント */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LightProbe|Sampling")
    TObjectPtr<ULightColorSamplerComponent> ColorSampler;

    /** DMX出力コンポーネント */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LightProbe|DMX")
    TObjectPtr<UDMXColorOutputComponent> DMXOutput;

    /** OSC出力コンポーネント (別PCのQLC+/DasLight等に送信) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LightProbe|OSC")
    TObjectPtr<UOSCColorOutputComponent> OSCOutput;

    // === 出力モード切替 ===

    /** DMX直接出力を使用するか */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Output")
    bool bUseDMXOutput = true;

    /** OSCネットワーク出力を使用するか (別PCへの送信用) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Output")
    bool bUseOSCOutput = false;

    // === 設定 ===

    /** キャプチャ解像度 (低いほど高速、高いほど精度向上) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Settings", meta = (ClampMin = "16", ClampMax = "512"))
    int32 CaptureResolution = 64;

    /** サンプリング更新レート (Hz) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Settings", meta = (ClampMin = "1", ClampMax = "120"))
    float SamplingRate = 30.0f;

    /** プローブが有効か */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Settings")
    bool bIsProbeActive = true;

    /** プローブ名 (識別用) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Settings")
    FString ProbeName = TEXT("LightProbe_01");

    /** ガンマ補正値 (1.0=リニア出力, 2.2=sRGB変換) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Color", meta = (ClampMin = "0.1", ClampMax = "5.0"))
    float GammaCorrection = 1.0f;

    /** 色温度オフセット (K) - リアルLEDとの色味調整用 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Color", meta = (ClampMin = "-3000", ClampMax = "3000"))
    float ColorTemperatureOffset = 0.0f;

    /** 彩度の乗数 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Color", meta = (ClampMin = "0.0", ClampMax = "3.0"))
    float SaturationMultiplier = 1.0f;

    /** 明度の乗数 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Color", meta = (ClampMin = "0.0", ClampMax = "3.0"))
    float BrightnessMultiplier = 1.0f;

    /** 色のみモード: 明度を正規化して色相・彩度だけを送信 (明度は照明側で制御) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Color")
    bool bColorOnly = false;

    /** 色のみモード時の最低明度 (暗いシーンで完全なブラックを送る閾値) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Color", meta = (ClampMin = "0.0", ClampMax = "0.1", EditCondition = "bColorOnly"))
    float ColorOnlyBlackThreshold = 0.01f;

    // === 出力 (読み取り専用) ===

    /** 現在のサンプリング結果色 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LightProbe|Output")
    FLinearColor CurrentSampledColor;

    /** DMX送信用に補正された色 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "LightProbe|Output")
    FLinearColor CorrectedOutputColor;

    // === メソッド ===

    /** プローブのON/OFF切り替え */
    UFUNCTION(BlueprintCallable, Category = "LightProbe")
    void SetProbeActive(bool bActive);

    /** 手動で1フレーム分のサンプリングを実行 */
    UFUNCTION(BlueprintCallable, Category = "LightProbe")
    void ForceSampleOnce();

    /** 現在のサンプリング色を取得 */
    UFUNCTION(BlueprintPure, Category = "LightProbe")
    FLinearColor GetCurrentColor() const { return CorrectedOutputColor; }

    /** プローブ名を取得 */
    UFUNCTION(BlueprintPure, Category = "LightProbe")
    FString GetProbeName() const { return ProbeName; }

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaTime) override;
    virtual void OnConstruction(const FTransform &Transform) override;

    /** エディタ(非PIE)でもTickを有効にする */
    virtual bool ShouldTickIfViewportsOnly() const override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent) override;
#endif

private:
    /** キャプチャ用レンダーターゲット */
    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTargetCube> CaptureRenderTarget;

    /** サンプリングタイマー */
    float SamplingTimer = 0.0f;

    /** キャプチャが初期化済みか */
    bool bCaptureInitialized = false;

    /** キャプチャの初期化 */
    void InitializeCapture();

    /** キャプチャの遅延初期化 (editor/runtimeどちらでも呼べる) */
    void EnsureCaptureInitialized();

    /** レンダーターゲットを作成/更新 */
    void UpdateRenderTarget();

    /** 色補正を適用 */
    FLinearColor ApplyColorCorrection(const FLinearColor &RawColor) const;

    /** 色温度をRGB変換 */
    static FLinearColor ColorTemperatureToRGB(float TempKelvin);
};
