// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LightSyncBlueprintLibrary.generated.h"

class ALightProbeActor;

/**
 * ULightSyncBlueprintLibrary
 *
 * Blueprint から LightSync 機能にアクセスするためのユーティリティ関数群。
 * レベルBPやウィジェットBPから簡単に呼び出せる。
 */
UCLASS()
class LIGHTSYNCDMX_API ULightSyncBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    /** ワールド内の全LightProbeActorを取得 */
    UFUNCTION(BlueprintCallable, Category = "LightSync", meta = (WorldContext = "WorldContextObject"))
    static TArray<ALightProbeActor *> GetAllLightProbes(const UObject *WorldContextObject);

    /** 全プローブを有効化/無効化 */
    UFUNCTION(BlueprintCallable, Category = "LightSync", meta = (WorldContext = "WorldContextObject"))
    static void SetAllProbesActive(const UObject *WorldContextObject, bool bActive);

    /** 全プローブをブラックアウト */
    UFUNCTION(BlueprintCallable, Category = "LightSync", meta = (WorldContext = "WorldContextObject"))
    static void BlackoutAllProbes(const UObject *WorldContextObject);

    /** マスターディマーを設定 */
    UFUNCTION(BlueprintCallable, Category = "LightSync", meta = (WorldContext = "WorldContextObject"))
    static void SetMasterDimmer(const UObject *WorldContextObject, float Dimmer);

    /** FLinearColor を DMX RGB 値 (0-255) に変換するユーティリティ */
    UFUNCTION(BlueprintPure, Category = "LightSync|Utility")
    static void LinearColorToDMXValues(const FLinearColor &Color, int32 &OutR, int32 &OutG, int32 &OutB);

    /** DMX RGB 値 (0-255) を FLinearColor に変換するユーティリティ */
    UFUNCTION(BlueprintPure, Category = "LightSync|Utility")
    static FLinearColor DMXValuesToLinearColor(int32 R, int32 G, int32 B);
};
