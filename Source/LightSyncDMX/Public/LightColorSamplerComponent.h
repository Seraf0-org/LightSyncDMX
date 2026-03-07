// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LightColorSamplerComponent.generated.h"

class UTextureRenderTargetCube;
class UTextureRenderTarget2D;

/**
 * ULightColorSamplerComponent
 *
 * RenderTargetCube の全面を読み取り、平均色を算出するコンポーネント。
 * GPU ReadBack を使用してレンダーターゲットからピクセルデータを取得する。
 */
UCLASS(ClassGroup = (LightSyncDMX), meta = (BlueprintSpawnableComponent, DisplayName = "Light Color Sampler"))
class LIGHTSYNCDMX_API ULightColorSamplerComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULightColorSamplerComponent();

    // === 設定 ===

    /** ダウンサンプリング解像度 (読み取りピクセル数を削減) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling", meta = (ClampMin = "2", ClampMax = "64"))
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

    /** 上面 (天井) の重み (0で無視, 1で等重み) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Face Weights", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float TopFaceWeight = 0.3f;

    /** 下面 (床) の重み */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Face Weights", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float BottomFaceWeight = 0.1f;

    /** 側面4面 (前後左右) の重み */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Face Weights", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float SideFaceWeight = 1.0f;

    /** トーンマッピングの露出値 (高いほど明るい部分が圧縮される) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Tone Mapping", meta = (ClampMin = "0.1", ClampMax = "10.0"))
    float ToneMappingExposure = 1.0f;

    // === 出力 ===

    /** 最新のサンプリング結果 (生の平均色) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling|Output")
    FLinearColor RawSampledColor;

    /** スムージング適用済みの色 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling|Output")
    FLinearColor SmoothedColor;

    /** 支配的な色方向 (例: 上からの光 vs 横からの光) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sampling|Output")
    FLinearColor DominantDirectionColor;

    // === メソッド ===

    /** CubeMapレンダーターゲットからサンプリング */
    UFUNCTION(BlueprintCallable, Category = "Sampling")
    void SampleFromRenderTarget(UTextureRenderTargetCube *InCubeRT);

    /** 現在のスムージング済み色を取得 */
    UFUNCTION(BlueprintPure, Category = "Sampling")
    FLinearColor GetSampledColor() const { return SmoothedColor; }

    /** 生のサンプリング色を取得 */
    UFUNCTION(BlueprintPure, Category = "Sampling")
    FLinearColor GetRawColor() const { return RawSampledColor; }

    /** サンプリング有効フラグ */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling")
    bool bSamplingEnabled = true;

protected:
    virtual void BeginPlay() override;

private:
    /** CubeMapの各面を読み取って平均色を計算 */
    FLinearColor ReadAverageColorFromCube(UTextureRenderTargetCube *CubeRT);

    /** 2DレンダーターゲットからピクセルデータをCPUに読み取り */
    FLinearColor ReadAverageFromPixels(const TArray<FFloat16Color> &Pixels, int32 Width, int32 Height);

    /** 色のスムージング (EMA: Exponential Moving Average) */
    FLinearColor SmoothColor(const FLinearColor &NewColor, const FLinearColor &PreviousColor, float Alpha);

    /** 初回サンプリングフラグ */
    bool bIsFirstSample = true;
};
