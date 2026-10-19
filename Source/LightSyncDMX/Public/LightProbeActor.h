// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/Scene.h"
#include "LightColorSamplerComponent.h"
#include "LightProbeActor.generated.h"

class ULightColorSamplerComponent;
class UDMXColorOutputComponent;
class UOSCColorOutputComponent;
class USceneCaptureComponentCube;
class UTextureRenderTargetCube;
class UStaticMeshComponent;
class UBillboardComponent;

/** 照明の色を何に合わせるか */
UENUM(BlueprintType)
enum class ELightSyncReferenceMode : uint8
{
    /**
     * LEDウォールの表示色に合わせる (ディスプレイ基準)。
     * 参照カメラと同じ露出・グレーディング・トーンカーブを通した後のリニア値で積分する。
     * 壁と照明がカメラ上で揃う。
     */
    Wall UMETA(DisplayName = "LED Wall (Display-Referred)"),

    /**
     * シーンの物理的な光に合わせる (シーン基準)。
     * トーンマップ前の放射輝度を、参照カメラ (または手動) の露出だけで正規化する。
     */
    Scene UMETA(DisplayName = "Scene Light (Scene-Referred)"),
};

/** ポストプロセス (露出・グレーディング・トーンカーブ) をどこから取るか */
UENUM(BlueprintType)
enum class ELightSyncPostProcessSource : uint8
{
    /** 参照カメラ (未設定なら手動露出) */
    ReferenceCamera UMETA(DisplayName = "Reference Camera / Manual"),

    /**
     * nDisplay の外側ビューポート (壁に出る環境) と同じ合成。
     * 参照カメラ (or 手動露出) をシーンとして、RootActor のカスタム PP とカラーグレーディングを重ねる。
     */
    NDisplay UMETA(DisplayName = "nDisplay Outer Viewport"),
};

/**
 * ALightProbeActor
 *
 * シーンに配置する光サンプリングプローブ。
 * 配置位置の周囲360°を SceneCaptureComponentCube でキャプチャし、
 * ゾーンごとの正規化照度 (ULightColorSamplerComponent) を DMX / OSC に出力する。
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

    /**
     * この PC が照明を送らない設定 (nDisplay の送信ノード以外など) でもキャプチャを続けるか。
     * false (既定) なら、ゲーム中は送らない PC でキャプチャしない (描画ノードの負荷を増やさない)。
     * 送らない PC で GetZoneColor() などの値を使うときだけ true にする。エディタのプレビューは常にキャプチャする
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Settings", AdvancedDisplay)
    bool bSampleWhenNotOutputting = false;

    /** プローブ無効化/削除時に出力先へブラックアウトを送る (false なら最後の色を保持) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Settings")
    bool bBlackoutWhenInactive = true;

    /** 出力 (DMX / OSC) を一時停止する。サンプリングは続ける (キャリブレーションのテスト点灯中に使う) */
    UPROPERTY(VisibleAnywhere, BlueprintReadWrite, Transient, Category = "LightProbe|Settings")
    bool bSuspendOutput = false;

    /** プローブ名 (識別用) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Settings")
    FString ProbeName = TEXT("LightProbe_01");

    // === 色の基準 ===

    /** 照明の色を何に合わせるか */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Reference")
    ELightSyncReferenceMode ReferenceMode = ELightSyncReferenceMode::Wall;

    /**
     * 参照カメラ (CameraComponent を持つアクター。ICVFX カメラや CineCameraActor)。
     * Wall: このカメラのポストプロセス (露出・グレーディング・トーンカーブ) をキャプチャに使う。
     * Scene: このカメラの手動露出で正規化する。
     * 未設定なら下の手動露出を使う。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Reference")
    TObjectPtr<AActor> ReferenceCamera;

    /** ポストプロセスの取得元 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Reference")
    ELightSyncPostProcessSource PostProcessSource = ELightSyncPostProcessSource::ReferenceCamera;

    /** nDisplay の RootActor (未設定ならレベル内の最初の RootActor) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Reference",
              meta = (EditCondition = "PostProcessSource == ELightSyncPostProcessSource::NDisplay"))
    TObjectPtr<AActor> NDisplayRootActor;

    /**
     * Per-Viewport グレーディングとカスタム PP を引くビューポート ID (例: VP_0)。
     * プローブが照らしたい方向の壁を映しているビューポートを指定する。空ならクラスタ全体のグレーディングのみ。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Reference",
              meta = (EditCondition = "PostProcessSource == ELightSyncPostProcessSource::NDisplay"))
    FString NDisplayViewportId;

    /** 直近に適用した nDisplay のレイヤー (診断用) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "LightProbe|Reference")
    FString NDisplayReport;

    /** 参照カメラが無いときの露出 (EV100)。UE の Manual 露出と同じ定義 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Reference", meta = (ClampMin = "-10", ClampMax = "20"))
    float ManualEV100 = 0.0f;

    /** 参照カメラが無いときの露出補正 (stop)。UE の Exposure Compensation と同じ定義 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Reference", meta = (ClampMin = "-15", ClampMax = "15"))
    float ManualExposureBias = 1.0f;

    /**
     * Wall モード: LEDウォールの表示特性 (信号値 → 光)。
     * nDisplay は sRGB でエンコードした信号を送るので、壁が sRGB/ガンマで光に戻す特性を選ぶ。
     * 実測で合わせる場合はキャリブレーションの壁パッチ測定で補正される。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Reference",
              meta = (EditCondition = "ReferenceMode == ELightSyncReferenceMode::Wall"))
    ELightSyncInputTransfer WallDisplayEOTF = ELightSyncInputTransfer::SRGB;

    /** 全ゾーン共通の出力ゲイン (リニア)。灯体ごとの強さはフィクスチャ側で調整する */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LightProbe|Reference", meta = (ClampMin = "0.0", ClampMax = "100.0"))
    float OutputGain = 1.0f;

    // === 統計 (LightSync.Stats で一覧) ===

    /** 実行したキューブキャプチャ数 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "LightProbe|Stats")
    int32 CapturesPerformed = 0;

    /** 前回の GPU 結果待ちのため省いたキャプチャ数 (省かないと結果が使われず GPU が無駄になる) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "LightProbe|Stats")
    int32 CapturesSkippedBusy = 0;

    /** 1 フレームのキャプチャ上限で次フレームに回した数 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "LightProbe|Stats")
    int32 CapturesDeferredBudget = 0;

    /** キャプチャ + ディスパッチのゲームスレッド時間の移動平均 (ms) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "LightProbe|Stats")
    float AvgCaptureGameThreadMs = 0.0f;

    /** 直近のキャプチャで使った露出倍率 (Scene モード) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "LightProbe|Reference")
    float EffectiveExposureScale = 1.0f;

    // === メソッド ===

    /** プローブのON/OFF切り替え */
    UFUNCTION(BlueprintCallable, Category = "LightProbe")
    void SetProbeActive(bool bActive);

    /** 手動で1フレーム分のサンプリングを実行 */
    UFUNCTION(BlueprintCallable, Category = "LightProbe")
    void ForceSampleOnce();

    /** 名前でゾーンの色を取得 (リニア、正規化照度) */
    UFUNCTION(BlueprintPure, Category = "LightProbe")
    bool GetZoneColor(FName ZoneName, FLinearColor &OutColor) const;

    /** 先頭の有効ゾーンの色 (一覧表示用) */
    UFUNCTION(BlueprintPure, Category = "LightProbe")
    FLinearColor GetCurrentColor() const;

    /** プローブ名を取得 */
    UFUNCTION(BlueprintPure, Category = "LightProbe")
    FString GetProbeName() const { return ProbeName; }

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaTime) override;
    virtual void OnConstruction(const FTransform &Transform) override;
    virtual void Destroyed() override;

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

    /** 使用中の出力 (DMX/OSC) にブラックアウトを送る */
    void BlackoutOutputs();

    /** エディタワールドのプローブが PIE 中に二重送信しないよう判定 */
    bool ShouldSkipForPIE() const;

    /** サブシステムのデバッグ描画 */
    void DrawDebugProbe() const;

    /** 基準モードに合わせて CaptureSource / ShowFlags / PostProcess を設定し、サンプラーへの文脈を返す */
    FLightSyncSampleContext ApplyCaptureSettings();

    /** 参照カメラの最終ポストプロセス設定を取得 */
    bool GetReferenceCameraPostProcess(FPostProcessSettings &OutSettings) const;

    /** 基準のポストプロセス (参照カメラ or 手動露出、nDisplay 選択時はレイヤーを重ねたもの) */
    FPostProcessSettings BuildReferencePostProcess();

    /** キャプチャして積分に回す。前回の結果待ち / フレームの上限ならしないで false */
    bool TryCaptureAndDispatch(bool bRespectBudget);

    /** ゾーンの結果を DMX / OSC に出す */
    void SendOutputs();

    /** nDisplay の PP の合成結果のキャッシュ (リフレクションを毎サンプル回さない) */
    FPostProcessSettings CachedNDisplayPP;
    double CachedNDisplayTime = -1.0;
    FString CachedNDisplayKey;

    /** 自動露出の警告を一度だけ出すためのフラグ */
    bool bWarnedAutoExposure = false;
};
