// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "LightSyncGPUSampler.h"
#include "LightSyncZones.h"
#include "LightSyncOCIO.h"
#include "LightColorSamplerComponent.generated.h"

class UTextureRenderTargetCube;
class UTextureRenderTarget2D;

/** サンプリング方式 */
UENUM(BlueprintType)
enum class ELightSyncSamplingMethod : uint8
{
    /** GPU Compute Shader + 非同期ReadBack (推奨、高速) */
    GPUCompute UMETA(DisplayName = "GPU Compute (Fast)"),

    /** CPU同期読み取り (フォールバック、低速。GPU と同じ式) */
    CPUSync UMETA(DisplayName = "CPU Sync (Fallback)")
};

/** プローブからサンプラーに渡す、キャプチャの解釈情報 */
struct FLightSyncSampleContext
{
    /** プローブの回転 (ゾーン方向をワールド空間に変換する) */
    FQuat ProbeRotation = FQuat::Identity;

    /** キャプチャ値 → 正規化値 の倍率 (露出) */
    float ExposureScale = 1.0f;

    /** キャプチャ (デコード後) が表示の色域 InputGamut の値か (false なら作業色空間) */
    bool bInputIsDisplayGamut = false;

    /** bInputIsDisplayGamut のときの色域 (OCIO なしの Wall モードは Rec.709) */
    ELightSyncDisplayGamut InputGamut = ELightSyncDisplayGamut::Rec709;

    /** キャプチャ値の伝達関数 (Wall モードは表示用にエンコード済みの値なので EOTF でデコードする) */
    ELightSyncInputTransfer InputTransfer = ELightSyncInputTransfer::Linear;

    /** デコード後に掛ける補正ガンマ (壁の実際の応答カーブをキャリブレーションで合わせる。1 = 補正なし) */
    float TransferGamma = 1.0f;

    /** InputTransfer = Gamma のときのガンマ */
    float DecodeGamma = 2.2f;

    /** InputTransfer = PQ のとき 1.0 とする明るさ (nits) */
    float PQReferenceWhiteNits = 100.0f;

    /** 積分の前に掛ける OCIO (無効なら掛けない) */
    FLightSyncOCIOPass OCIO;
};

/**
 * ULightColorSamplerComponent
 *
 * キューブマップをゾーンごとに立体角で積分し、正規化照度 (リニア) を求めるコンポーネント。
 * 計算式は LightSyncAverageCS.usf / LightSyncZones::IntegrateCube を参照。
 */
UCLASS(ClassGroup = (LightSyncDMX), meta = (BlueprintSpawnableComponent, DisplayName = "Light Color Sampler"))
class LIGHTSYNCDMX_API ULightColorSamplerComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    ULightColorSamplerComponent();

    // === 設定 ===

    /** サンプリング方式 (GPU推奨) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling")
    ELightSyncSamplingMethod SamplingMethod = ELightSyncSamplingMethod::GPUCompute;

    /** サンプリング有効フラグ */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling")
    bool bSamplingEnabled = true;

    /** ゾーン (最大 8 個まで有効)。出力側は Name で参照する */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Zones", meta = (TitleProperty = "Name"))
    TArray<FLightSyncZone> Zones;

    /**
     * 方向を分け合うゾーン同士の境界の鋭さ。
     * 大きいほど「最も近いゾーンが全部持つ」に近づき、小さいほどなめらかに按分する。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling|Zones", meta = (ClampMin = "1.0", ClampMax = "32.0"))
    float PartitionSharpness = 4.0f;

    /** 時間方向のスムージングの時定数 (秒)。0 = スムージングなし。フレームレートに依存しない */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sampling", meta = (ClampMin = "0.0", ClampMax = "10.0", Units = "s"))
    float SmoothingTimeConstant = 0.15f;

    // === 出力 ===

    /** ゾーンごとの結果 (スムージング済み、リニア、正規化照度) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sampling|Output")
    TArray<FLightSyncZoneResult> ZoneResults;

    /** 最も明るい方向 (ワールド空間) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sampling|Output")
    FVector KeyDirection = FVector::UpVector;

    // === メソッド ===

    /** CubeMapレンダーターゲットからサンプリング (結果は 1〜2 フレーム後に ZoneResults へ反映) */
    void SampleFromRenderTarget(UTextureRenderTargetCube *InCubeRT, const FLightSyncSampleContext &Context);

    /** 新しいサンプルを受け付けられるか (前回の GPU 結果待ちなら false → キャプチャしても無駄になる) */
    bool CanDispatch() const;

    /** キャプチャ済みのキューブを積分に回す */
    void Dispatch(UTextureRenderTargetCube *InCubeRT, const FLightSyncSampleContext &Context);

    /** 結果が届いていれば ZoneResults に反映して true (毎フレーム呼ぶ。サンプリング間隔を待たずに出力できる) */
    bool PollNewResult();

    /** 直近の結果の遅延 (ディスパッチ → 結果, ミリ秒) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sampling|Stats")
    float LastResultLatencyMs = 0.0f;

    /** 直近の結果のスムージング前の値 (ZoneResults と同じ並び。変化の検出に使う) */
    const TArray<FLinearColor> &GetLastTargetColors() const { return LastTargetColors; }

    /** 名前でゾーンの色を取得 */
    UFUNCTION(BlueprintPure, Category = "Sampling")
    bool GetZoneColor(FName ZoneName, FLinearColor &OutColor) const;

    /** 有効なゾーン (結果の並び順) */
    TArray<FLightSyncZone> GetActiveZones() const;

    /**
     * 検証: 直近にサンプリングしたキューブを CPU で読み出して同じ式で積分し、GPU の生結果と比べる。
     * 静止したシーンで使う (GPU 結果は 1〜2 フレーム前のキャプチャなので)。重いので検証専用。
     * @return 比較できたら true。OutMaxRelError は全ゾーン・全チャンネルの最大相対誤差
     */
    UFUNCTION(BlueprintCallable, Category = "Sampling|Debug")
    bool ValidateAgainstCPU(FString &OutReport, float &OutMaxRelError) const;

    /** 既定ゾーンに戻す */
    UFUNCTION(BlueprintCallable, Category = "Sampling")
    void ResetZonesToDefault();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void OnUnregister() override;
    virtual void BeginDestroy() override;

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent) override;
#endif

private:
    TUniquePtr<FLightSyncGPUSampler> GPUSampler;

    void InitializeGPUSampler();
    void ReleaseGPUSampler();

    /** 新しい積分結果を ZoneResults に反映 (色変換・露出・スムージング) */
    void ApplyNewResult(const LightSyncZones::FResult &Result, const FLightSyncSampleContext &Context);

    /** 直近の生結果と、それを計算したときの設定・入力 (検証用) */
    LightSyncZones::FResult LastRawResult;
    LightSyncZones::FSetup LastSetup;
    TWeakObjectPtr<UTextureRenderTargetCube> LastRenderTarget;
    TWeakObjectPtr<UTextureRenderTarget2D> LastOCIOAtlas;

    /** ディスパッチ時の文脈 (結果の反映に使う) と時刻 */
    FLightSyncSampleContext PendingContext;
    double DispatchTime = 0.0;

    /** 前回の結果を反映した時刻 */
    double LastResultTime = -1.0;

    /** 前回のゾーン名の並び (変化したらスムージングをリセット) */
    TArray<FName> LastZoneNames;

    /** スムージング前の値 */
    TArray<FLinearColor> LastTargetColors;
};
