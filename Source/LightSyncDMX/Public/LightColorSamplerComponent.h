// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LightSyncGPUSampler.h"
#include "LightColorSamplerComponent.generated.h"

class UTextureRenderTargetCube;
class UTextureRenderTarget2D;

/** サンプリング方式 */
UENUM(BlueprintType)
enum class ELightSyncSamplingMethod : uint8
{
    /** GPU Compute Shader + 非同期ReadBack (推奨、高速) */
    GPUCompute UMETA(DisplayName = "GPU Compute (Fast)"),
    
    /** CPU同期読み取り (フォールバック、低速) */
    CPUSync UMETA(DisplayName = "CPU Sync (Legacy)")
};

/**
 * ULightColorSamplerComponent
 *
 * RenderTargetCube の全面を読み取り、平均色を算出するコンポーネント。
 * GPU Compute Shader による高速な非同期サンプリングをサポート。
 */
UCLASS(ClassGroup = (LightSyncDMX), meta = (BlueprintSpawnableComponent, DisplayName = "Light Color Sampler"))
class LIGHTSYNCDMX_API ULightColorSamplerComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULightColorSamplerComponent();
    virtual ~ULightColorSamplerComponent();

    // === サンプリング方式 ===

    /** サンプリング方式 (GPU推奨) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling")
    ELightSyncSamplingMethod SamplingMethod = ELightSyncSamplingMethod::GPUCompute;

    // === 設定 ===

    /** ダウンサンプリング解像度 (CPUSyncモードのみ使用) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|CPU Fallback", meta = (ClampMin = "2", ClampMax = "64", EditCondition = "SamplingMethod == ELightSyncSamplingMethod::CPUSync"))
    int32 DownsampleResolution = 8;

    /** スムージング用の補間速度 (色のちらつき防止, 1.0=即時反映) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling", meta = (ClampMin = "0.01", ClampMax = "1.0"))
    float SmoothingAlpha = 0.5f;

    /** HDR値をどこでクランプするか */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling", meta = (ClampMin = "1.0", ClampMax = "100.0"))
    float HDRClampValue = 10.0f;

    /** 暗部の閾値 (これ以下の輝度は無視) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling", meta = (ClampMin = "0.0", ClampMax = "0.1"))
    float DarkThreshold = 0.001f;

    /** 直接光フィルター: これ以上の輝度は直接光源とみなして除外 (VP向け間接光サンプリング) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Indirect Light", meta = (ClampMin = "0.1", ClampMax = "10.0"))
    float DirectLightThreshold = 1.5f;

    /** 直接光フィルターを有効にする (VP用途で間接光のみ取得したい場合に有効化) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Indirect Light")
    bool bFilterDirectLight = true;

    /** 輝度加重の指数 (高いほど明るい光源の影響が強い, 1.0=線形, 0=均等) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Luminance Weighting", meta = (ClampMin = "0.0", ClampMax = "4.0"))
    float LuminanceExponent = 0.5f;

    /** 上面 (天井) の重み (0で無視, 1で等重み) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Face Weights", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float TopFaceWeight = 0.3f;

    /** 下面 (床) の重み */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Face Weights", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float BottomFaceWeight = 0.1f;

    /** 側面4面 (前後左右) の重み */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Face Weights", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float SideFaceWeight = 1.0f;

    /** トーンマッピングの露出値 (低いほど白飛び抑制、高いほど明るく) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Tone Mapping", meta = (ClampMin = "0.1", ClampMax = "10.0"))
    float ToneMappingExposure = 0.5f;

    /** 彩度ブースト (1.0=そのまま、2.0=彩度2倍。白飛び対策に有効) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Tone Mapping", meta = (ClampMin = "0.0", ClampMax = "3.0"))
    float SaturationBoost = 2.0f;

    // === 出力 ===

    /** 最新のサンプリング結果 (生の平均色) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling|Output")
    FLinearColor RawSampledColor;

    /** スムージング適用済みの色 (全体平均) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling|Output")
    FLinearColor SmoothedColor;

    /** 上方向からの光の色 (天井のPavoTube用) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling|Output|Directional")
    FLinearColor TopColor;

    /** 横方向からの光の色 (側面のPavoTube用) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling|Output|Directional")
    FLinearColor SideColor;

    /** 最も明るい光源の色 (スポットライト用) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling|Output|Directional")
    FLinearColor DominantColor;

    // === メソッド ===

    /** CubeMapレンダーターゲットからサンプリング */
    UFUNCTION(BlueprintCallable, Category = "Sampling")
    void SampleFromRenderTarget(UTextureRenderTargetCube *InCubeRT);

    /** 現在のスムージング済み色を取得 (全体平均) */
    UFUNCTION(BlueprintPure, Category = "Sampling")
    FLinearColor GetSampledColor() const { return SmoothedColor; }

    /** 上方向の色を取得 */
    UFUNCTION(BlueprintPure, Category = "Sampling")
    FLinearColor GetTopColor() const { return TopColor; }

    /** 横方向の色を取得 */
    UFUNCTION(BlueprintPure, Category = "Sampling")
    FLinearColor GetSideColor() const { return SideColor; }

    /** 最も明るい光源の色を取得 */
    UFUNCTION(BlueprintPure, Category = "Sampling")
    FLinearColor GetDominantColor() const { return DominantColor; }

    /** 生のサンプリング色を取得 */
    UFUNCTION(BlueprintPure, Category = "Sampling")
    FLinearColor GetRawColor() const { return RawSampledColor; }

    /** サンプリング有効フラグ */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling")
    bool bSamplingEnabled = true;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    // === GPU Compute 方式 ===
    
    /** GPU サンプラー */
    TUniquePtr<FLightSyncGPUSampler> GPUSampler;

    /** GPU サンプラーの初期化 */
    void InitializeGPUSampler();

    // === CPU Sync 方式 (フォールバック) ===

    /** CubeMapの各面を読み取って平均色を計算 (CPU同期方式) */
    FLinearColor ReadAverageColorFromCube_CPUSync(UTextureRenderTargetCube *CubeRT);

    /** 2DレンダーターゲットからピクセルデータをCPUに読み取り */
    FLinearColor ReadAverageFromPixels(const TArray<FFloat16Color> &Pixels, int32 Width, int32 Height);

    /** 色のスムージング (EMA: Exponential Moving Average) */
    FLinearColor SmoothColor(const FLinearColor &NewColor, const FLinearColor &PreviousColor, float Alpha);

    /** 初回サンプリングフラグ */
    bool bIsFirstSample = true;

    /** スムージング用の前フレーム値 */
    FLinearColor PrevTopColor = FLinearColor::Black;
    FLinearColor PrevSideColor = FLinearColor::Black;
    FLinearColor PrevDominantColor = FLinearColor::Black;
};
