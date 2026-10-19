// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LightSyncZones.generated.h"

/**
 * ゾーンの形状。
 * どの形状も「一様な放射輝度 1 の環境」で出力 1.0 になるよう正規化した照度を返す。
 */
UENUM(BlueprintType)
enum class ELightSyncZoneShape : uint8
{
    /** Direction を向いた面が受ける照度 (半球、コサイン重み)。天井・正面ライト向け */
    Irradiance = 0 UMETA(DisplayName = "Irradiance (Hemisphere)"),

    /** Direction を軸とした水平リング。軸に垂直な面の照度を全方位で平均 (側面ライトをまとめる用) */
    Ring = 1 UMETA(DisplayName = "Ring (Around Axis)"),

    /** Direction 周りのコーン内だけの照度 (コサイン重み) */
    Cone = 2 UMETA(DisplayName = "Cone"),

    /** 全方位の平均放射輝度 */
    Sphere = 3 UMETA(DisplayName = "Sphere (Average)"),

    /** 最も明るい方向を自動で探し、その周りのコーンの照度 (キーライト/スポット用) */
    Key = 4 UMETA(DisplayName = "Key (Brightest Direction)"),

    /** 出力しない。このコーンの方向を全ゾーンから除外する (LEDウォール自体が照らす方向など) */
    Exclude = 5 UMETA(DisplayName = "Exclude (e.g. LED Wall)"),
};

/** キャプチャ値の伝達関数 (texel ごとにリニア光へデコードしてから積分する) */
UENUM(BlueprintType)
enum class ELightSyncInputTransfer : uint8
{
    Linear = 0 UMETA(DisplayName = "Linear"),
    SRGB = 1 UMETA(DisplayName = "sRGB"),
    Gamma22 = 2 UMETA(DisplayName = "Gamma 2.2"),
    Gamma24 = 3 UMETA(DisplayName = "Gamma 2.4 (BT.1886)"),
};

/** ゾーン定義 (プローブ単位) */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FLightSyncZone
{
    GENERATED_BODY()

    /** 出力側 (DMX / OSC) から参照する名前 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone")
    FName Name = TEXT("Zone");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone")
    bool bEnabled = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone")
    ELightSyncZoneShape Shape = ELightSyncZoneShape::Irradiance;

    /** 方向 / 軸 (プローブのローカル空間)。Sphere / Key では無視 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone",
              meta = (EditCondition = "Shape != ELightSyncZoneShape::Sphere && Shape != ELightSyncZoneShape::Key"))
    FVector Direction = FVector::UpVector;

    /** コーンの半角 (度)。Cone / Key / Exclude で使用 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone", meta = (ClampMin = "1", ClampMax = "180", UIMin = "1", UIMax = "180",
              EditCondition = "Shape == ELightSyncZoneShape::Cone || Shape == ELightSyncZoneShape::Key || Shape == ELightSyncZoneShape::Exclude"))
    float ConeHalfAngle = 30.0f;

    /** コーン境界のぼかし幅 (度)。境界をまたぐ光源で出力が跳ねないように */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone", meta = (ClampMin = "0", ClampMax = "90",
              EditCondition = "Shape == ELightSyncZoneShape::Cone || Shape == ELightSyncZoneShape::Key || Shape == ELightSyncZoneShape::Exclude"))
    float EdgeSoftness = 10.0f;

    /**
     * 他の「方向を分け合う」ゾーンと方向を分割する (二重計上しない)。
     * Irradiance / Ring / Cone のみ有効。Sphere / Key は常に独立。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Zone",
              meta = (EditCondition = "Shape == ELightSyncZoneShape::Irradiance || Shape == ELightSyncZoneShape::Ring || Shape == ELightSyncZoneShape::Cone"))
    bool bShareDirections = true;

    bool OutputsColor() const { return bEnabled && Shape != ELightSyncZoneShape::Exclude; }
};

/** ゾーンの計測結果 */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FLightSyncZoneResult
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Zone")
    FName Name;

    /**
     * 正規化照度 (リニア、作業色空間)。1.0 = 基準白の一様な環境から受ける照度。
     * 上限でクランプしていない (強さの比率を保つため)。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Zone")
    FLinearColor Color = FLinearColor::Black;

    /** このゾーンが受け持つ立体角 (sr)。4π = 全方位 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Zone")
    float CoverageSr = 0.0f;
};

namespace LightSyncZones
{
    /** シェーダーと共有する最大ゾーン数 */
    static constexpr int32 MaxZones = 8;

    /** GPU / CPU 積分器に渡すゾーン (ワールド空間) */
    struct FZoneDesc
    {
        FVector3f Direction = FVector3f(0.0f, 0.0f, 1.0f);
        ELightSyncZoneShape Shape = ELightSyncZoneShape::Irradiance;
        float CosOuter = 0.0f;   // コーン外縁の cos
        float CosInner = 0.0f;   // コーン内縁の cos
        bool bShared = false;
    };

    struct FSetup
    {
        TArray<FZoneDesc> Zones;
        float PartitionSharpness = 4.0f;
        FVector3f LuminanceCoeffs = FVector3f(0.2126f, 0.7152f, 0.0722f);
        /** Key 方向の精密化に使うコーンの cos (最初の Key ゾーンの外縁)。2 = Key なし */
        float KeyRefineCos = 2.0f;
        /** キャプチャ値の伝達関数 */
        ELightSyncInputTransfer InputTransfer = ELightSyncInputTransfer::Linear;
    };

    /** 積分結果 (Zones と同じ並び) */
    struct FResult
    {
        TArray<FVector4f> Zones;  // rgb = 正規化照度, a = 受け持つ立体角
        FVector3f KeyDirection = FVector3f(0.0f, 0.0f, 1.0f);
        float KeyLuminance = 0.0f;
    };

    /** UE ゾーン定義 → 積分器用 (プローブの回転でワールド空間へ) */
    LIGHTSYNCDMX_API FSetup BuildSetup(TConstArrayView<FLightSyncZone> Zones, const FQuat& ProbeRotation,
                                       float PartitionSharpness, const FVector3f& LuminanceCoeffs,
                                       ELightSyncInputTransfer InputTransfer = ELightSyncInputTransfer::Linear);

    /** 伝達関数のデコード (シェーダーの DecodeTransfer と同じ) */
    LIGHTSYNCDMX_API float DecodeTransfer(ELightSyncInputTransfer Transfer, float Value);

    /** キューブ面 texel (UV 0-1) → ワールド方向 (D3D キューブマップ規約) */
    LIGHTSYNCDMX_API FVector3f FaceUVToDirection(int32 Face, float U, float V);

    /** キューブ面 texel の立体角 */
    LIGHTSYNCDMX_API float TexelSolidAngle(float U, float V, int32 Size);

    /**
     * CPU 版の積分器。GPU シェーダー (LightSyncAverageCS.usf) と同じ式・同じブロック分割で計算する。
     * CPU フォールバックと、GPU 結果の検証 (テストオラクル) に使う。
     *
     * @param FaceTexels 6 面 × Size×Size のリニアRGB (行優先, 面順 +X,-X,+Y,-Y,+Z,-Z)
     */
    LIGHTSYNCDMX_API FResult IntegrateCube(const TArray<FLinearColor> (&FaceTexels)[6], int32 Size, const FSetup& Setup);

    /** 既定のゾーン構成 (Top / Side / Average / Key) */
    LIGHTSYNCDMX_API TArray<FLightSyncZone> MakeDefaultZones();
}
