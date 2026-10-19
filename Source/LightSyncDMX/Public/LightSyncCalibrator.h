// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "LightSyncFixtureProfile.h"
#include "LightSyncCalibrator.generated.h"

class ALightProbeActor;
class ALightSyncCalibrationPatch;
class UTexture;
class UTextureRenderTarget2D;

/** カメラの記録方式 (コード値 → シーンリニア) */
UENUM(BlueprintType)
enum class ELightSyncCameraTransfer : uint8
{
    Linear UMETA(DisplayName = "Linear"),
    SRGB UMETA(DisplayName = "sRGB"),
    Rec709 UMETA(DisplayName = "Rec.709"),
    ArriLogC3 UMETA(DisplayName = "ARRI LogC3 (EI 800)"),
    ArriLogC4 UMETA(DisplayName = "ARRI LogC4"),
    SonySLog3 UMETA(DisplayName = "Sony S-Log3"),
};

UENUM(BlueprintType)
enum class ELightSyncCalibrationStepKind : uint8
{
    /** 灯体を消して壁も黒 (環境光・黒レベル) */
    FixtureOff,
    /** エミッタ 1 個を 100% (Dimmer があれば 100%) */
    FixtureEmitter,
    /** 全エミッタを同じレベル (調光カーブ用。Dimmer があれば Dimmer をそのレベルに) */
    FixtureAllLevel,
    /** 壁に全面パッチを表示 (灯体は消灯) */
    WallPatch,
};

/** 測定 1 回分 */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FLightSyncCalibrationStep
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    FString Label;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    ELightSyncCalibrationStepKind Kind = ELightSyncCalibrationStepKind::FixtureOff;

    /** FixtureEmitter: エミッタ番号 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    int32 EmitterIndex = INDEX_NONE;

    /** FixtureAllLevel: DMX レベル (0-1) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    float Level = 1.0f;

    /** WallPatch: パッチ色 (1.0 = PatchIntensity) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    FLinearColor PatchColor = FLinearColor::Black;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    bool bMeasured = false;

    /** グレーカードのカメラのコード値 (0-1、カメラの記録方式のまま) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    FLinearColor CameraCode = FLinearColor::Black;

    /** WallPatch: 記録時にプローブが測った基準値 (正規化照度) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    FLinearColor ProbeReference = FLinearColor::Black;
};

/**
 * ULightSyncCalibrator
 *
 * カメラだけでの キャリブレーション。
 *
 *  灯体: 消灯 / 各エミッタ 100% / 全エミッタ 25-50-75-100% を点灯し、グレーカードのカメラ値を記録
 *        → エミッタの応答 (M_fix の列) と調光カーブをプロファイルに書く
 *  壁  : 全面パッチ 黒 / 赤 / 緑 / 青 / 白 を表示し、グレーカードのカメラ値とプローブの測定値を記録
 *        → 基準 → カメラの 3x3 (M_ref) をステージキャリブレーションに書く
 *
 * カメラの露出・ホワイトバランス・グレーカードの位置は全測定で固定すること。
 * 強さの基準を合わせるため、壁の測定はグレーカードを壁に向けて壁の近くで、
 * 灯体の測定は被写体の位置で灯体に向けて行う。
 */
UCLASS(BlueprintType)
class LIGHTSYNCDMX_API ULightSyncCalibrator : public UObject
{
    GENERATED_BODY()

public:
    // === 設定 ===

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    TObjectPtr<ALightProbeActor> Probe;

    /** キャリブレーションするフィクスチャ (プローブの DMX Output → Fixture Mappings の番号) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    int32 FixtureIndex = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration|Camera")
    ELightSyncCameraTransfer CameraTransfer = ELightSyncCameraTransfer::Linear;

    /** カメラ映像 (Media Texture など)。設定するとグレーカードの範囲を自動で平均できる */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration|Camera")
    TObjectPtr<UTexture> CameraFeed;

    /** グレーカードの範囲 (映像に対する 0-1) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration|Camera")
    FVector2D FeedROIMin = FVector2D(0.45, 0.45);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration|Camera")
    FVector2D FeedROIMax = FVector2D(0.55, 0.55);

    /**
     * 映像テクスチャが sRGB テクスチャとしてデコードされてサンプリングされるか。
     * true なら sRGB でエンコードし直してから CameraTransfer で線形化する (コード値に戻すため)。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration|Camera")
    bool bFeedSampledAsSRGB = true;

    /** 壁パッチの明るさ (エミッシブ)。壁の信号が白飛びしない程度にする */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration|Wall", meta = (ClampMin = "0.001", ClampMax = "1000"))
    float PatchIntensity = 1.0f;

    /** 壁パッチの基準値を読むゾーン (Sphere 形状を推奨) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration|Wall")
    FName ReferenceZone = TEXT("Average");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    TArray<FLightSyncCalibrationStep> FixtureSteps;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    TArray<FLightSyncCalibrationStep> WallSteps;

    /** いま壁に表示している WallSteps の番号 (なければ INDEX_NONE) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Calibration")
    int32 ActiveWallStep = INDEX_NONE;

    /** いま点灯している FixtureSteps の番号 (なければ INDEX_NONE) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Calibration")
    int32 ActiveFixtureStep = INDEX_NONE;

    /** 直近のフィット結果 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Calibration")
    FString LastReport;

    // === 操作 ===

    /** フィクスチャのプロファイルに合わせて測定手順を作り直す (記録は消える) */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    void BuildSteps();

    /** 灯体のテスト点灯 (プローブの出力を止め、壁は黒パッチ) */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    bool ShowFixtureStep(int32 Index);

    /** 壁パッチを表示 (プローブの出力を止め、灯体は消灯) */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    bool ShowWallStep(int32 Index);

    /** テスト表示を終了 (パッチを消し、プローブの出力を再開) */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    void EndTestOutput();

    /** カメラ映像の ROI を平均してコード値 (0-1) を返す */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    bool CaptureCameraFeed(FLinearColor &OutCameraCode);

    /** 測定値を記録 */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    bool RecordFixtureMeasurement(int32 Index, FLinearColor CameraCode);

    /**
     * 測定値を記録。そのステップを表示中ならプローブの現在の基準値も記録する
     * (表示中でなく、基準値もまだ無いときは失敗する)
     */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    bool RecordWallMeasurement(int32 Index, FLinearColor CameraCode);

    /** 灯体の測定からプロファイルを求めて Target に書き込む (null ならフィクスチャのプロファイル) */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    bool FitFixture(ULightSyncFixtureProfile *Target, FString &OutReport);

    /** 壁の測定から M_ref を求めて Target に書き込む (null ならプローブの DMX 出力のキャリブレーション) */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    bool FitStage(ULightSyncStageCalibration *Target, FString &OutReport);

#if WITH_EDITOR
    /** フィクスチャのプロファイルアセットを作って割り当てる (/Game/LightSync/Profiles/)。既にあればそれを返す */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    ULightSyncFixtureProfile *CreateProfileAssetForFixture();

    /** ステージキャリブレーションのアセットを作り、プローブの DMX / OSC 出力に割り当てる。既にあればそれを返す */
    UFUNCTION(BlueprintCallable, Category = "Calibration")
    ULightSyncStageCalibration *CreateStageCalibrationAsset();
#endif

    // === フィット (純粋関数。テスト用に公開) ===

    static FVector3d LinearizeCamera(ELightSyncCameraTransfer Transfer, const FLinearColor &Code);

    /** 灯体の測定 → プロファイル (エミッタ応答と調光カーブを更新) */
    static bool FitFixtureFromSteps(const FLightSyncFixtureProfileData &Base, const TArray<FLightSyncCalibrationStep> &Steps,
                                    ELightSyncCameraTransfer Transfer, FLightSyncFixtureProfileData &OutProfile, FString &OutReport);

    /** 壁の測定 → M_ref の列 */
    static bool FitStageFromSteps(const TArray<FLightSyncCalibrationStep> &Steps, ELightSyncCameraTransfer Transfer,
                                  FLinearColor &OutRed, FLinearColor &OutGreen, FLinearColor &OutBlue, FString &OutReport);

    /** プロファイルから灯体の測定手順を作る */
    static TArray<FLightSyncCalibrationStep> MakeFixtureSteps(const FLightSyncFixtureProfileData &Profile);

    /** 壁の測定手順 */
    static TArray<FLightSyncCalibrationStep> MakeWallSteps();

private:
    bool GetFixtureProfile(FLightSyncFixtureProfileData &OutProfile) const;
    ALightSyncCalibrationPatch *EnsurePatch(const FLinearColor &Color);

    UPROPERTY(Transient)
    TObjectPtr<ALightSyncCalibrationPatch> Patch;

    UPROPERTY(Transient)
    TObjectPtr<UTextureRenderTarget2D> FeedReadTarget;
};
