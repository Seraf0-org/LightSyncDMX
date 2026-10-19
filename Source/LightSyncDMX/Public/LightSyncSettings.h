// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "LightSyncSettings.generated.h"

class UWorld;

/** どの PC (UE インスタンス) が照明を送信するか */
UENUM(BlueprintType)
enum class ELightSyncOutputPolicy : uint8
{
    /**
     * nDisplay のクラスタノードでは「送信ノード」だけが送り、エディタ / 単体実行では常に送る。
     * エディタ (Multi-User のオペレーター) とクラスタを同時に動かす場合は、どちらかを止める設定にすること。
     */
    Auto UMETA(DisplayName = "Auto (cluster: output node only / otherwise: this PC)"),

    /** nDisplay のクラスタノードの「送信ノード」だけが送る。エディタ / 単体実行からは送らない */
    ClusterNodeOnly UMETA(DisplayName = "nDisplay Output Node Only"),

    /** エディタ / 単体実行だけが送る。クラスタノードからは送らない */
    NonClusterOnly UMETA(DisplayName = "Editor / Standalone Only"),

    /** どこからも送らない (コマンドラインの -LightSyncOutput=On では送る) */
    Disabled UMETA(DisplayName = "Disabled"),
};

/**
 * Project Settings → Plugins → LightSync DMX
 *
 * 送信する PC の決め方。PC ごとに変えたいときはコマンドライン引数で上書きする:
 *   -LightSyncOutput=On   … この PC から必ず送る
 *   -LightSyncOutput=Off  … この PC からは送らない
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "LightSync DMX"))
class LIGHTSYNCDMX_API ULightSyncSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    ULightSyncSettings();

    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

    /** どの PC が照明 (DMX / OSC) を送信するか */
    UPROPERTY(Config, EditAnywhere, Category = "Output")
    ELightSyncOutputPolicy OutputPolicy = ELightSyncOutputPolicy::Auto;

    /** nDisplay で送信するクラスタノード ID (空ならプライマリノード) */
    UPROPERTY(Config, EditAnywhere, Category = "Output")
    FString OutputClusterNodeId;

    /** 1 フレームに行うキューブキャプチャの上限 (プローブが多いときにフレーム時間を平らにする)。0 = 無制限 */
    UPROPERTY(Config, EditAnywhere, Category = "Performance", meta = (ClampMin = "0", ClampMax = "64"))
    int32 MaxCapturesPerFrame = 2;

    /** アプリ終了時に、UE の DMX ポートを通さず直接 Art-Net / sACN でブラックアウトを送る */
    UPROPERTY(Config, EditAnywhere, Category = "Output")
    bool bDirectBlackoutOnExit = true;
};

namespace LightSyncOutput
{
    /** この UE インスタンスが照明を送信してよいか (理由つき)。結果は短時間キャッシュする */
    LIGHTSYNCDMX_API bool CanOutput(const UWorld* World, FString* OutReason = nullptr);

    /** nDisplay のクラスタノードとして動いているか (-dc_cluster)。ノード ID を返す */
    LIGHTSYNCDMX_API bool IsClusterNode(FString* OutNodeId = nullptr);

    /** 判定のキャッシュを捨てる (設定変更時など) */
    LIGHTSYNCDMX_API void InvalidateCache();
}
