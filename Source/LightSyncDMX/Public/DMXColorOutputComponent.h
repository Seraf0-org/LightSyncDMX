// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DMXProtocolCommon.h"
#include "LightSyncZones.h"
#include "LightSyncFixtureProfile.h"
#include "DMXColorOutputComponent.generated.h"

class UDMXLibrary;

/**
 * FDMXFixtureMapping
 * 1つのDMXフィクスチャへのマッピング情報
 */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FDMXFixtureMapping
{
    GENERATED_BODY()

    /** フィクスチャ名 (識別用) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX")
    FString FixtureName = TEXT("LED_01");

    /** このフィクスチャが再現するゾーン (プローブの Color Sampler → Zones の Name) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX")
    FName Zone = TEXT("Average");

    /** DMXユニバース番号 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX", meta = (ClampMin = "1", ClampMax = "64000"))
    int32 Universe = 1;

    /** 開始チャンネル (1-512) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX", meta = (ClampMin = "1", ClampMax = "512"))
    int32 StartChannel = 1;

    /**
     * 灯体プロファイル (エミッタの応答・チャンネル配置・調光カーブ)。
     * 未設定なら下の内蔵プリセット (理想的な Rec.709 エミッタ) を使う。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX")
    TObjectPtr<ULightSyncFixtureProfile> Profile;

    /** 内蔵プリセット (Profile 未設定時) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX", meta = (EditCondition = "Profile == nullptr"))
    ELightSyncColorMode ColorMode = ELightSyncColorMode::RGB;

    /** 有効フラグ */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX")
    bool bEnabled = true;

    /**
     * 強さのゲイン (リニア)。目標の正規化照度 1.0 を、この灯体の何倍の出力で再現するか。
     * 灯体の距離・大きさの違いを吸収する。キャリブレーションで求める。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX", meta = (ClampMin = "0.0", ClampMax = "100.0"))
    float BrightnessScale = 1.0f;
};

/** フィクスチャごとの直近の出力状態 (モニター・診断用) */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FLightSyncFixtureStatus
{
    GENERATED_BODY()

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX")
    FString FixtureName;

    /** 灯体の色域外 (色相を保って彩度を下げた / CCT 灯体のレンジ外) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX")
    bool bOutOfGamut = false;

    /** 明るさが上限を超えた (等倍で下げた) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX")
    bool bClipped = false;

    /** 全体の明るさ (0-1) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX")
    float Intensity = 0.0f;

    /** CCT 灯体の色温度 (K) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX")
    float CCT = 0.0f;

    /** 送信した DMX 値 (先頭チャンネルから) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX")
    TArray<uint8> Values;
};

/**
 * UDMXColorOutputComponent
 *
 * ゾーンの目標色を、灯体プロファイルとソルバで DMX 値に変換して送信するコンポーネント。
 * UEのDMXProtocolプラグインを使用してArt-Net / sACN で送信。
 * 1つのプローブから複数のフィクスチャに同時出力可能。
 */
UCLASS(ClassGroup = (LightSyncDMX), meta = (BlueprintSpawnableComponent, DisplayName = "DMX Color Output"))
class LIGHTSYNCDMX_API UDMXColorOutputComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UDMXColorOutputComponent();

    // === 設定 ===

    /**
     * DMXライブラリへの参照 (任意)。
     * 設定した場合はライブラリの Output Ports のみに送信する。未設定なら Project Settings の全出力ポートに送信。
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|Config")
    TSoftObjectPtr<UDMXLibrary> DMXLibrary;

    /** ステージのキャリブレーション (基準 → カメラ)。未設定なら単位行列 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|Config")
    TObjectPtr<ULightSyncStageCalibration> StageCalibration;

    /** 出力先フィクスチャのマッピングリスト */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|Config", meta = (TitleProperty = "FixtureName"))
    TArray<FDMXFixtureMapping> FixtureMappings;

    /** DMX出力が有効か */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|Config")
    bool bDMXOutputEnabled = true;

    /** マスターディマー (0-1)。目標の強さに掛ける (色は変えない) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|Config", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MasterDimmer = 1.0f;

    /** 最後に送信した色 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX|Output")
    FLinearColor LastSentColor;

    /** フィクスチャごとの直近の出力 (FixtureMappings と同じ並び) */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "DMX|Output")
    TArray<FLightSyncFixtureStatus> FixtureStatus;

    /** 送信成功フレームカウンタ */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX|Output")
    int64 SentFrameCount = 0;

    // === メソッド ===

    /** 色をDMXとして全フィクスチャに送信 (ゾーンに関係なく同じ色) */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void SendColor(const FLinearColor &Color);

    /** 各フィクスチャに、マッピングで指定したゾーンの色を送信 */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void SendZoneColors(const TArray<FLightSyncZoneResult> &ZoneResults);

    /** 特定のフィクスチャのみに色を送信 */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void SendColorToFixture(const FLinearColor &Color, int32 FixtureIndex);

    /** 特定のフィクスチャに生の DMX 値を送る (キャリブレーションのテスト点灯用。値は先頭チャンネルから) */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void SendRawToFixture(int32 FixtureIndex, const TArray<uint8> &Values);

    /** 全フィクスチャをブラックアウト */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void Blackout();

    /** フィクスチャマッピングを追加 */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void AddFixtureMapping(const FDMXFixtureMapping &Mapping);

    /** フィクスチャマッピングを削除 */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void RemoveFixtureMapping(int32 Index);

    /** 内蔵プリセットが使用するチャンネル数 */
    UFUNCTION(BlueprintPure, Category = "DMX")
    static int32 GetChannelCount(ELightSyncColorMode Mode);

    /** マッピングが使用するチャンネル数 (プロファイル優先) */
    UFUNCTION(BlueprintPure, Category = "DMX")
    static int32 GetMappingChannelCount(const FDMXFixtureMapping &Mapping);

    /** マッピングがユニバース内 (1-512) に収まっているか */
    UFUNCTION(BlueprintPure, Category = "DMX")
    static bool IsMappingInRange(const FDMXFixtureMapping &Mapping);

    /** マッピングが使うプロファイル (アセット or 内蔵プリセット) */
    static FLightSyncFixtureProfileData GetMappingProfile(const FDMXFixtureMapping &Mapping);

    /** 目標色 → 灯体 1 台分の出力 (MasterDimmer / BrightnessScale / キャリブレーション適用済み) */
    FLightSyncFixtureOutput ComputeFixtureOutput(const FDMXFixtureMapping &Mapping, const FLinearColor &Color) const;

    /** FLinearColorからDMXチャンネル値 (絶対チャンネル番号 → 値) へ変換 */
    TMap<int32, uint8> ColorToDMXChannels(const FDMXFixtureMapping &Mapping, const FLinearColor &Color) const;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    /** 目標色を計算して送信し、状態を記録 */
    void SendFixture(int32 MappingIndex, const FLinearColor &Color);

    /** オフセット順の値を絶対チャンネルにして送信 */
    void SendValues(const FDMXFixtureMapping &Mapping, const TArray<uint8> &Values);

    /** 送信先の出力ポートを取得 (DMXLibrary 指定時はそのポートのみ) */
    void GatherOutputPorts(TArray<FDMXOutputPortSharedRef> &OutPorts);

    /** ロード済みの DMXLibrary (TSoftObjectPtr の毎フレーム解決を避ける) */
    UPROPERTY(Transient)
    TObjectPtr<UDMXLibrary> LoadedDMXLibrary;

    /** DMX出力ポート未設定警告を一度だけ出すためのフラグ */
    bool bWarnedNoOutputPorts = false;

    /** DMXLibrary に出力ポートが無い警告を一度だけ出すためのフラグ */
    bool bWarnedLibraryNoPorts = false;

    /** 範囲外 (512ch超え) 警告済みのフィクスチャ名 */
    TSet<FString> WarnedOutOfRangeFixtures;

    /** 存在しないゾーンを指定している警告済みのフィクスチャ名 */
    TSet<FString> WarnedMissingZoneFixtures;
};
