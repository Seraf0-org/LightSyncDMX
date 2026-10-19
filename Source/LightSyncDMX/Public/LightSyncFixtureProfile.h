// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "LightSyncColorMath.h"
#include "LightSyncFixtureProfile.generated.h"

/**
 * 内蔵プリセット (よくあるチャンネル配置)。
 * 実際の灯体の特性はプロファイルアセット (ULightSyncFixtureProfile) で上書きする。
 */
UENUM(BlueprintType)
enum class ELightSyncColorMode : uint8
{
    /** R, G, B の3チャンネル */
    RGB UMETA(DisplayName = "RGB (3ch)"),
    /** R, G, B, W の4チャンネル */
    RGBW UMETA(DisplayName = "RGBW (4ch)"),
    /** R, G, B, Amber, White の5チャンネル */
    RGBAW UMETA(DisplayName = "RGBAW (5ch)"),
    /** Dimmer, R, G, B の4チャンネル (Dimmer先頭) */
    DRGB UMETA(DisplayName = "Dimmer+RGB (4ch)"),
    /** 色温度 + 明度 の2チャンネル */
    CCT UMETA(DisplayName = "CCT+Brightness (2ch)"),
    /** 10ch: CCT/GreenMagenta/Dimmer/Crossfade/R/G/B/W/Strobe/Reserved (RGBW モードで使用) */
    CCTRGBW10CH UMETA(DisplayName = "CCT & RGBW (10ch)"),
    /**
     * Nanlite PavoSlim (60C/120C/240C など) の DMX モード「CCT & RGBW 8bit」(11ch):
     * Dimmer / CCT / G-M / Crossfade / R / G / B / W / Strobe / Reserved / Reserved。
     * Crossfade = 255 (RGBW のみ)、G-M = 132 (ニュートラル)、Strobe = 0 (OFF) を固定で送る。
     * 本体の "DMX Reduced Channels" が ON の場合は末尾 3ch が無くなるので、プロファイルアセットで削る
     */
    NanlitePavoSlimRGBW11CH UMETA(DisplayName = "Nanlite PavoSlim CCT & RGBW 8bit (11ch)"),
};

/** 灯体の色の作り方 */
UENUM(BlueprintType)
enum class ELightSyncFixtureColorModel : uint8
{
    /** エミッタ (R/G/B/W/A...) の混色をソルバで解く */
    Emitters UMETA(DisplayName = "Emitter Mix (RGB/RGBW/...)"),
    /** 色温度 + Tint (Duv) + 明るさで指定する白色灯 */
    CCT UMETA(DisplayName = "CCT + Tint"),
};

/** DMX チャンネルの役割 */
UENUM(BlueprintType)
enum class ELightSyncChannelFunction : uint8
{
    /** エミッタのレベル (EmitterIndex 番目) */
    Emitter,
    /** マスター調光 */
    Dimmer,
    /** 色温度 (CCT モデルのみ。Emitters モデルでは FixedValue を送る) */
    CCT,
    /** Green/Magenta (Duv)。CCT モデルのみ。Emitters モデルでは FixedValue を送る */
    Tint,
    /** 固定値 (ストロボ OFF、モード切替、予備など) */
    Fixed,
};

/** 調光カーブの種類 (DMX レベル → 光出力) */
UENUM(BlueprintType)
enum class ELightSyncDimmingCurve : uint8
{
    Linear,
    Gamma,
    /** 計測点の区分線形 (X = DMX レベル 0-1, Y = 光出力 0-1) */
    LUT,
};

/** エミッタ (LED の色ごと) */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FLightSyncEmitter
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Emitter")
    FName Name = TEXT("R");

    /**
     * このエミッタを 100% で点けたときのカメラのリニア RGB (グレーカードの読み値)。
     * 未計測なら理想値 (R = (1,0,0) など) のままでよい。全エミッタで同じ距離・露出で測ること。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Emitter")
    FLinearColor CameraResponse = FLinearColor(1.0f, 0.0f, 0.0f);

    /** 白エミッタか (ソルバが優先して使う) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Emitter")
    bool bIsWhite = false;
};

/** DMX チャンネル 1 個 (16bit なら 2 チャンネル分) */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FLightSyncFixtureChannel
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channel")
    FName Label = TEXT("R");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channel")
    ELightSyncChannelFunction Function = ELightSyncChannelFunction::Emitter;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channel", meta = (ClampMin = "0", ClampMax = "7",
              EditCondition = "Function == ELightSyncChannelFunction::Emitter"))
    int32 EmitterIndex = 0;

    /** 16bit (Coarse + Fine の 2 チャンネル) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channel")
    bool b16Bit = false;

    /** Fixed のとき、または Emitters モデルの CCT / Tint チャンネルで送る値 (8bit: 0-255, 16bit: 0-65535) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Channel", meta = (ClampMin = "0", ClampMax = "65535"))
    int32 FixedValue = 0;

    int32 GetWidth() const { return b16Bit ? 2 : 1; }
};

/** 灯体プロファイルの中身 (アセットと内蔵プリセットで共通) */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FLightSyncFixtureProfileData
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture")
    ELightSyncFixtureColorModel ColorModel = ELightSyncFixtureColorModel::Emitters;

    /** エミッタ (Emitters モデル)。CCT モデルでは 1 個目を「白の応答」として明るさの基準に使う */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture", meta = (TitleProperty = "Name"))
    TArray<FLightSyncEmitter> Emitters;

    /** 白エミッタをどれだけ優先するか (0 = 最小エネルギー解, 1 = 白を最大限使う)。色の一致が常に優先 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture", meta = (ClampMin = "0", ClampMax = "1"))
    float WhitePreference = 0.5f;

    /** DMX チャンネル配置 (先頭から順に並ぶ。16bit は 2ch 使う) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture|Channels", meta = (TitleProperty = "Label"))
    TArray<FLightSyncFixtureChannel> Channels;

    /**
     * 調光カーブ (DMX レベル → 光出力)。
     * Dimmer チャンネルがあれば Dimmer に、なければ各エミッタに適用する。
     * Dimmer がある場合のエミッタ値は混色比 (リニア) として扱う。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture|Dimming")
    ELightSyncDimmingCurve DimmingCurve = ELightSyncDimmingCurve::Linear;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture|Dimming", meta = (ClampMin = "0.1", ClampMax = "5.0",
              EditCondition = "DimmingCurve == ELightSyncDimmingCurve::Gamma"))
    float DimmingGamma = 2.2f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture|Dimming",
              meta = (EditCondition = "DimmingCurve == ELightSyncDimmingCurve::LUT"))
    TArray<FVector2D> DimmingLUT;

    /** CCT チャンネルの最小値 (0) に対応する色温度 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture|CCT", meta = (ClampMin = "1000", ClampMax = "25000"))
    float CCTMinKelvin = 2700.0f;

    /** CCT チャンネルの最大値に対応する色温度 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture|CCT", meta = (ClampMin = "1000", ClampMax = "25000"))
    float CCTMaxKelvin = 6500.0f;

    /** Tint チャンネルの最小値 (0) に対応する Duv (マゼンタ側) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture|CCT", meta = (ClampMin = "-0.1", ClampMax = "0.1"))
    float TintMinDuv = -0.02f;

    /** Tint チャンネルの最大値に対応する Duv (グリーン側) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture|CCT", meta = (ClampMin = "-0.1", ClampMax = "0.1"))
    float TintMaxDuv = 0.02f;

    /** 使用チャンネル数 (16bit は 2) */
    int32 GetChannelCount() const;

    bool HasDimmer() const;

    LightSyncColor::FDimmingCurve MakeDimmingCurve() const;
};

/**
 * 灯体プロファイル (アセット)。
 * 灯体の機種ごと (理想的には個体ごと) に作り、キャリブレーションでエミッタの応答と調光カーブを埋める。
 */
UCLASS(BlueprintType)
class LIGHTSYNCDMX_API ULightSyncFixtureProfile : public UDataAsset
{
    GENERATED_BODY()

public:
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture")
    FString FixtureModel;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Fixture", meta = (ShowOnlyInnerProperties))
    FLightSyncFixtureProfileData Profile;

    /** 内蔵プリセットで初期化する */
    UFUNCTION(CallInEditor, Category = "Fixture")
    void ResetToRGBPreset();

    UFUNCTION(BlueprintCallable, Category = "Fixture")
    void InitializeFromPreset(ELightSyncColorMode Mode);
};

/**
 * ステージのキャリブレーション (基準 → カメラ)。
 * 基準 (壁パッチ / シーン) の R, G, B 原色をカメラで見たリニア RGB = M_ref の列。
 * 既定は単位行列 (カメラ = 作業色空間)。キャリブレーションで埋める。
 */
UCLASS(BlueprintType)
class LIGHTSYNCDMX_API ULightSyncStageCalibration : public UDataAsset
{
    GENERATED_BODY()

public:
    /** 基準の R 原色 (作業色空間 (1,0,0)) をカメラで見たリニア RGB */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    FLinearColor ReferenceRed = FLinearColor(1.0f, 0.0f, 0.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    FLinearColor ReferenceGreen = FLinearColor(0.0f, 1.0f, 0.0f);

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration")
    FLinearColor ReferenceBlue = FLinearColor(0.0f, 0.0f, 1.0f);

    /**
     * 壁の応答の補正ガンマ。Wall モードで、表示特性 (sRGB 等) でデコードした値にさらに掛ける。
     * LED プロセッサの実際のカーブが想定とずれている分を、壁の明るさを変えたパッチの測定から求める。1 = 補正なし
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Calibration", meta = (ClampMin = "0.2", ClampMax = "5.0"))
    float ReferenceGamma = 1.0f;

    LightSyncColor::FMatrix3x3 GetReferenceToCamera() const;
};

/** 灯体 1 台分の計算結果 */
struct LIGHTSYNCDMX_API FLightSyncFixtureOutput
{
    /** チャンネルごとの DMX 値 (先頭チャンネルからのオフセット順) */
    TArray<uint8> Values;

    /** エミッタの光出力の割合 (ソルバの解、0-1)。CCT モデルでは空 */
    TArray<double> EmitterOutputs;

    /** 全体の明るさ (0-1、Dimmer があればその光出力) */
    double Intensity = 0.0;

    /** CCT モデルの色温度 / Duv */
    double CCT = 0.0;
    double Duv = 0.0;

    /** 灯体の色域外だったので白方向へ彩度を下げた (CCT モデルではレンジ外) */
    bool bOutOfGamut = false;

    /** 明るさが上限を超えたので等倍で下げた */
    bool bClipped = false;
};

namespace LightSyncFixture
{
    /** 内蔵プリセット */
    LIGHTSYNCDMX_API FLightSyncFixtureProfileData MakePreset(ELightSyncColorMode Mode);

    /** 内蔵プリセット (一度だけ作って使い回す。毎フレームのコピーを避ける) */
    LIGHTSYNCDMX_API const FLightSyncFixtureProfileData& GetPreset(ELightSyncColorMode Mode);

    /**
     * 目標色 (作業色空間のリニア、正規化照度) → DMX 値。
     *
     *   Emitters: c = M_ref · (Gain · T) をカメラ空間の目標にしてエミッタの光出力を解く
     *             → Dimmer があれば Dimmer = 最大出力、エミッタ = 比率 / なければ各エミッタに調光カーブの逆
     *   CCT     : XYZ から CCT + Duv、明るさ = カメラ空間の輝度 / 白エミッタの輝度
     */
    LIGHTSYNCDMX_API FLightSyncFixtureOutput Compute(const FLightSyncFixtureProfileData& Profile, const FLinearColor& TargetWorking,
                                                     const LightSyncColor::FMatrix3x3& ReferenceToCamera, double Gain);

    /** 消灯 (エミッタ / Dimmer = 0、固定値はそのまま) */
    LIGHTSYNCDMX_API FLightSyncFixtureOutput ComputeBlackout(const FLightSyncFixtureProfileData& Profile);

    /**
     * キャリブレーション用のテスト点灯 (調光カーブを通さない生の DMX レベル)。
     * EmitterLevels[i] をエミッタ i のチャンネルに、DimmerLevel を Dimmer に送る。
     * CCT モデルでは CCT = 中央、Tint = 0 (Duv)、明るさ = DimmerLevel。固定値はそのまま。
     */
    LIGHTSYNCDMX_API FLightSyncFixtureOutput ComputeTestPattern(const FLightSyncFixtureProfileData& Profile,
                                                                const TArray<double>& EmitterLevels, double DimmerLevel);
}
