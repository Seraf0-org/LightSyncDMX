// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "DMXColorOutputComponent.generated.h"

class UDMXLibrary;

/**
 * ELightSyncColorMode
 * DMXチャンネルへの色の割り当て方式
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
    /** 10ch: CCT/GreenMagenta/Dimmer/Crossfade/R/G/B/W/Strobe/Reserved */
    CCTRGBW10CH UMETA(DisplayName = "CCT & RGBW (10ch)"),
};

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

    /** DMXユニバース番号 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX", meta = (ClampMin = "1", ClampMax = "64000"))
    int32 Universe = 1;

    /** 開始チャンネル (1-512) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX", meta = (ClampMin = "1", ClampMax = "512"))
    int32 StartChannel = 1;

    /** カラーモード */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX")
    ELightSyncColorMode ColorMode = ELightSyncColorMode::RGB;

    /** 有効フラグ */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX")
    bool bEnabled = true;

    /** 個別の明度オフセット (0.0 - 2.0) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float BrightnessScale = 1.0f;

    /** 10chモード: Ch2 Green/Magenta 制御値 (0-255, 0=Neutral/No Effect) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|CCTRGBW10ch", meta = (ClampMin = "0", ClampMax = "255", EditCondition = "ColorMode==ELightSyncColorMode::CCTRGBW10CH"))
    int32 CCTRGBW10_GreenMagenta = 0;

    /** 10chモード: Ch4 CCT↔RGBW クロスフェード (0=CCT, 255=RGBW) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|CCTRGBW10ch", meta = (ClampMin = "0", ClampMax = "255", EditCondition = "ColorMode==ELightSyncColorMode::CCTRGBW10CH"))
    int32 CCTRGBW10_Crossfade = 255;

    /** 10chモード: Ch9 ストロボ値 (0=OFF) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|CCTRGBW10ch", meta = (ClampMin = "0", ClampMax = "255", EditCondition = "ColorMode==ELightSyncColorMode::CCTRGBW10CH"))
    int32 CCTRGBW10_Strobe = 0;

    /** 10chモード: Ch10 予備チャンネル値 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|CCTRGBW10ch", meta = (ClampMin = "0", ClampMax = "255", EditCondition = "ColorMode==ELightSyncColorMode::CCTRGBW10CH"))
    int32 CCTRGBW10_Reserved10 = 0;
};

/**
 * UDMXColorOutputComponent
 *
 * サンプリングされた色をDMXプロトコルで送信するコンポーネント。
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

    /** DMXライブラリへの参照 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|Config")
    TSoftObjectPtr<UDMXLibrary> DMXLibrary;

    /** 出力先フィクスチャのマッピングリスト */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|Config")
    TArray<FDMXFixtureMapping> FixtureMappings;

    /** DMX出力が有効か */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|Config")
    bool bDMXOutputEnabled = true;

    /** マスターディマー値 (0-255 に対応) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DMX|Config", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float MasterDimmer = 1.0f;

    /** 最後に送信した色 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX|Output")
    FLinearColor LastSentColor;

    /** 送信成功フレームカウンタ */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DMX|Output")
    int64 SentFrameCount = 0;

    // === メソッド ===

    /** 色をDMXとして送信 */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void SendColor(const FLinearColor &Color);

    /** 特定のフィクスチャのみに色を送信 */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void SendColorToFixture(const FLinearColor &Color, int32 FixtureIndex);

    /** 全フィクスチャをブラックアウト */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void Blackout();

    /** フィクスチャマッピングを追加 */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void AddFixtureMapping(const FDMXFixtureMapping &Mapping);

    /** フィクスチャマッピングを削除 */
    UFUNCTION(BlueprintCallable, Category = "DMX")
    void RemoveFixtureMapping(int32 Index);

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    /** 色をDMXチャンネル値に変換して送信 */
    void SendDMXData(const FDMXFixtureMapping &Mapping, const FLinearColor &Color);

    /** FLinearColorからDMXバイト配列へ変換 */
    TMap<int32, uint8> ColorToDMXChannels(const FDMXFixtureMapping &Mapping, const FLinearColor &Color) const;

    /** RGB → RGBW 変換 */
    void RGBtoRGBW(float R, float G, float B, float &OutR, float &OutG, float &OutB, float &OutW) const;

    /** RGB → 色温度(CCT) + 明度 変換 */
    void RGBtoCCTBrightness(float R, float G, float B, float &OutCCT, float &OutBrightness) const;
};
