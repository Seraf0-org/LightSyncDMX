// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Scene.h"

class AActor;
class UWorld;

/**
 * LightSyncNDisplay
 *
 * nDisplay の外側ビューポート (LEDウォールに出る環境) のポストプロセスを再現する。
 *
 * nDisplay モジュールにはリンクせず、リフレクションで RootActor の設定を読む
 * (nDisplay を使わないプロジェクトに nDisplay を強制しないため)。
 * 合成ロジックは UE 5.7 の DisplayClusterViewportConfigurationHelpers_Postprocess.cpp と同じ:
 *
 *   Start (カスタム PP) → シーン (参照カメラ / 手動露出) → Override → Final → FinalPerViewport
 *
 *   FinalPerViewport = Entire Cluster カラーグレーディング と Per-Viewport カラーグレーディング の合成
 *     (露出補正・オフセット等は加算、彩度・コントラスト・ガンマ・ゲインは乗算、
 *      WhiteTemp は加算して 1 枚増えるごとに -6500、TemperatureType は後勝ち。重みは積)
 *   各レイヤーは FSceneView::OverridePostProcessSettings と同じく、数値は重みで補間、それ以外は置き換え。
 */
namespace LightSyncNDisplay
{
    /** FSceneView::OverridePostProcessSettings と同じ合成。Dest の bOverride も立てる */
    LIGHTSYNCDMX_API void OverridePostProcess(FPostProcessSettings& Dest, const FPostProcessSettings& Src, float Weight);

    /** nDisplay の RootActor か (クラス名で判定) */
    LIGHTSYNCDMX_API bool IsRootActor(const AActor* Actor);

    /** ワールド内の最初の nDisplay RootActor */
    LIGHTSYNCDMX_API AActor* FindRootActor(UWorld* World);

    /** RootActor の全ビューポート ID (クラスタノード順) */
    LIGHTSYNCDMX_API TArray<FString> GetViewportIds(const AActor* RootActor);

    /**
     * 外側ビューポートの最終ポストプロセスを作る。
     * @param ViewportId   per-viewport グレーディングとカスタム PP を引くビューポート (空ならクラスタ全体のグレーディングのみ)
     * @param ScenePP      シーン側のポストプロセス (参照カメラ / 手動露出)
     * @param OutReport    適用したレイヤーの説明
     * @return RootActor の設定を読めたら true
     */
    LIGHTSYNCDMX_API bool BuildOuterViewportPostProcess(const AActor* RootActor, const FString& ViewportId, const FPostProcessSettings& ScenePP,
                                                        FPostProcessSettings& OutPP, FString& OutReport);
}
