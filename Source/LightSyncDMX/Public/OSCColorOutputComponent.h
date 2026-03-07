// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "OSCColorOutputComponent.generated.h"

class UOSCClient;

/**
 * EOSCMessageFormat
 * OSCメッセージの送信フォーマット
 */
UENUM(BlueprintType)
enum class EOSCMessageFormat : uint8
{
    /** /lightsync/{probename}/rgb  [float, float, float] (0.0-1.0) */
    FloatRGB UMETA(DisplayName = "Float RGB (0.0-1.0)"),

    /** /lightsync/{probename}/rgb  [int, int, int] (0-255) */
    IntRGB255 UMETA(DisplayName = "Int RGB (0-255)"),

    /** /lightsync/{probename}/color [float, float, float, float] (R,G,B,A 0.0-1.0) */
    FloatRGBA UMETA(DisplayName = "Float RGBA (0.0-1.0)"),

    /** QLC+ 互換: /probename/rgb  [int] (単一のRGBパック値) */
    QLCPlusCompat UMETA(DisplayName = "QLC+ Compatible"),

    /** DasLight 互換: /dmx/universe/channel [int] (チャンネル毎に個別送信) */
    DasLightCompat UMETA(DisplayName = "DasLight Compatible"),

    /** カスタムアドレスにfloat3で送信 */
    Custom UMETA(DisplayName = "Custom Address"),
};

/**
 * FOSCTargetConfig
 * 送信先PC/アプリの設定
 */
USTRUCT(BlueprintType)
struct LIGHTSYNCDMX_API FOSCTargetConfig
{
    GENERATED_BODY()

    /** ターゲット名 (識別用) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC")
    FString TargetName = TEXT("QLC+ PC");

    /** 送信先IPアドレス */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC")
    FString IPAddress = TEXT("192.168.1.100");

    /** 送信先ポート番号 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC", meta = (ClampMin = "1", ClampMax = "65535"))
    int32 Port = 7700;

    /** 送信フォーマット */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC")
    EOSCMessageFormat Format = EOSCMessageFormat::FloatRGB;

    /** OSCアドレスのプレフィックス (例: /lightsync) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC")
    FString AddressPrefix = TEXT("/lightsync");

    /** DasLight互換モード時のUniverse番号 */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC",
              meta = (EditCondition = "Format==EOSCMessageFormat::DasLightCompat", ClampMin = "1", ClampMax = "64000"))
    int32 DMXUniverse = 1;

    /** DasLight互換モード時の開始チャンネル */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC",
              meta = (EditCondition = "Format==EOSCMessageFormat::DasLightCompat", ClampMin = "1", ClampMax = "512"))
    int32 DMXStartChannel = 1;

    /** カスタムモード時のフルOSCアドレス */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC",
              meta = (EditCondition = "Format==EOSCMessageFormat::Custom"))
    FString CustomAddress = TEXT("/custom/color");

    /** 有効フラグ */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC")
    bool bEnabled = true;
};

/**
 * UOSCColorOutputComponent
 *
 * サンプリングした色をOSCプロトコルで別PCに送信するコンポーネント。
 * QLC+, DasLight, 他のOSC対応照明ソフトウェアに色データを送信可能。
 *
 * ネットワーク経由で送信するため、UEのPCとDMXインターフェースのPCが
 * 別マシンでも動作する。
 */
UCLASS(ClassGroup = (LightSyncDMX), meta = (BlueprintSpawnableComponent, DisplayName = "OSC Color Output"))
class LIGHTSYNCDMX_API UOSCColorOutputComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UOSCColorOutputComponent();

    // === 設定 ===

    /** 送信先ターゲットの一覧 (複数PCに同時送信可能) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC|Config")
    TArray<FOSCTargetConfig> Targets;

    /** OSC出力が有効か */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC|Config")
    bool bOSCOutputEnabled = true;

    /** プローブ名 (OSCアドレスのパスとして使用) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC|Config")
    FString OSCProbeName = TEXT("probe01");

    /** 送信レート制限 (Hz) - OSCパケット数を抑える */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC|Config", meta = (ClampMin = "1", ClampMax = "120"))
    float OSCSendRate = 30.0f;

    /** バンドル送信を使うか (複数値をまとめて1パケットで送信) */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "OSC|Config")
    bool bUseBundleMessage = true;

    // === 出力状態 ===

    /** 最後に送信した色 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OSC|Output")
    FLinearColor LastSentColor;

    /** 送信成功カウンタ */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OSC|Output")
    int64 SentMessageCount = 0;

    /** 接続状態 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "OSC|Output")
    bool bIsConnected = false;

    // === メソッド ===

    /** 色をOSCで送信 (全ターゲットに送信) */
    UFUNCTION(BlueprintCallable, Category = "OSC")
    void SendColor(const FLinearColor &Color);

    /** 特定のターゲットにのみ色を送信 */
    UFUNCTION(BlueprintCallable, Category = "OSC")
    void SendColorToTarget(const FLinearColor &Color, int32 TargetIndex);

    /** 追加のカスタムOSCメッセージを送信 (Blueprintから自由に使用) */
    UFUNCTION(BlueprintCallable, Category = "OSC")
    void SendCustomOSCFloat(const FString &Address, float Value);

    /** 追加のカスタムOSCメッセージを送信 (整数値) */
    UFUNCTION(BlueprintCallable, Category = "OSC")
    void SendCustomOSCInt(const FString &Address, int32 Value);

    /** 全ターゲットにブラックアウト送信 */
    UFUNCTION(BlueprintCallable, Category = "OSC")
    void Blackout();

    /** ターゲットを追加 */
    UFUNCTION(BlueprintCallable, Category = "OSC")
    void AddTarget(const FOSCTargetConfig &Config);

    /** OSCクライアントを再接続 */
    UFUNCTION(BlueprintCallable, Category = "OSC")
    void ReconnectAll();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    /** ターゲット毎のOSCクライアント */
    UPROPERTY(Transient)
    TArray<TObjectPtr<UOSCClient>> OSCClients;

    /** OSCクライアントの作成/初期化 */
    void InitializeClients();

    /** OSCクライアントの遅延初期化 (エディタ/ランタイムどちらでも呼べる) */
    void EnsureClientsInitialized();

    /** OSCクライアントの破棄 */
    void DestroyClients();

    /** 特定のターゲットに色データを送信する内部メソッド */
    void SendToTarget(int32 TargetIndex, const FLinearColor &Color);

    /** フォーマットに応じたOSCメッセージ構築と送信 */
    void SendFormattedMessage(UOSCClient *Client, const FOSCTargetConfig &Config, const FLinearColor &Color);

    /** QLC+ 互換フォーマットで送信 */
    void SendQLCPlusFormat(UOSCClient *Client, const FOSCTargetConfig &Config, const FLinearColor &Color);

    /** DasLight 互換フォーマットで送信 */
    void SendDasLightFormat(UOSCClient *Client, const FOSCTargetConfig &Config, const FLinearColor &Color);

    /** 送信レート制御タイマー */
    float SendRateTimer = 0.0f;
};
