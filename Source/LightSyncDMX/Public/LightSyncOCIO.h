// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "OpenColorIORendering.h"

class UTextureRenderTarget2D;
struct FOpenColorIOColorConversionSettings;

/**
 * 積分の前にキャプチャへ掛ける OCIO 変換。
 *
 * nDisplay が OCIO を使うとき、外側ビューポートはトーンカーブなしのリニア値 (露出・グレーディング適用後) に
 * OCIO の変換を掛け、その結果 (表示用のコード値) を壁へ送る。プローブも同じ変換を同じシェーダーで掛ける:
 *   キューブ (FinalColorHDR) → 6 面を横に並べたアトラス → OCIO → Atlas (6N × N)
 * その後、texel ごとに壁の EOTF でデコードして積分する。
 */
struct FLightSyncOCIOPass
{
    /** OCIO のシェーダーと LUT (ゲームスレッドで取得し、レンダースレッドへコピーして使う) */
    FOpenColorIORenderPassResources Resources;

    /** OCIO 後の値を置く RGBA16F のアトラス (6N × N)。CPU の検証でも読む */
    TWeakObjectPtr<UTextureRenderTarget2D> Atlas;

    bool IsValid() const { return Resources.IsValid() && Atlas.IsValid(); }
};

namespace LightSyncOCIO
{
    /** 変換が設定されているか (構成アセットがあり、変換元・変換先が決まっている) */
    LIGHTSYNCDMX_API bool IsConfigured(const FOpenColorIOColorConversionSettings& Settings);

    /** 表示用の短い説明 (例 "simple.config: Utility - Linear - sRGB → LED Walls - sRGB / PQ") */
    LIGHTSYNCDMX_API FString Describe(const FOpenColorIOColorConversionSettings& Settings);
}
