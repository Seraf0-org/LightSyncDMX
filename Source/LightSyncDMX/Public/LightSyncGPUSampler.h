// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UTextureRenderTargetCube;
class FRHIGPUBufferReadback; // RHIGPUReadback.h をインクルードするのは .cpp 側

/**
 * FLightSyncGPUSampler
 *
 * CubeMap から輝度加重平均色を計算するサンプラー。
 *
 * GPU Compute Shader (SM5+) を使用して非同期にサンプリングし、
 * 1〜2 フレーム後にゲームスレッドで結果を取得できる。
 * GPU 使用不可時は CPU 同期方式にフォールバックする。
 *
 * 使い方:
 *   毎フレーム DispatchSampling() を呼んでディスパッチし、
 *   その直後に TryGetResult() で前フレーム以前の結果を取得する。
 */
class LIGHTSYNCDMX_API FLightSyncGPUSampler
{
public:
    FLightSyncGPUSampler();
    ~FLightSyncGPUSampler();

    void Initialize();
    void Release();

    /** CubeMap のサンプリングを GPU (or CPU) にディスパッチ */
    void DispatchSampling(UTextureRenderTargetCube* CubeRT);

    /**
     * サンプリング結果を取得する。
     * GPU 結果が準備できていれば true を返し、まだ未準備でも
     * キャッシュ済みの前フレーム結果があれば true を返す。
     */
    bool TryGetResult(FLinearColor& OutAverageColor,
                      FLinearColor& OutTopColor,
                      FLinearColor& OutSideColor,
                      FLinearColor& OutDominantColor);

    /** サンプリングパラメータ */
    struct FSamplingParams
    {
        float LuminanceExponent    = 0.5f;
        float DarkThreshold        = 0.001f;
        float HDRClamp             = 10.0f;
        float DirectLightThreshold = 1.5f;
        bool  bFilterDirectLight   = true;
        float TopFaceWeight        = 0.3f;
        float BottomFaceWeight     = 0.1f;
        float SideFaceWeight       = 1.0f;
        float ToneMappingExposure  = 0.5f;
        float SaturationBoost      = 2.0f;
        int32 DownsampleResolution = 8;
    };

    void SetParams(const FSamplingParams& InParams) { Params = InParams; }
    void SetForceCPUSync(bool bInForceCPUSync);
    bool IsInitialized() const { return bInitialized; }

private:
    FSamplingParams Params;

    // キャッシュ済み結果
    FLinearColor CachedAverageColor  = FLinearColor::Black;
    FLinearColor CachedTopColor      = FLinearColor::Black;
    FLinearColor CachedSideColor     = FLinearColor::Black;
    FLinearColor CachedDominantColor = FLinearColor::Black;

    bool bInitialized      = false;
    bool bDispatched       = false; // GPU ディスパッチが進行中 (ゲームスレッドのみ書き込み)
    bool bResultReady      = false; // キャッシュに有効な結果がある
    bool bGPUPathAvailable = false;
    bool bForceCPUSync     = false;

    // GPU Async Readback バッファ (12 × FVector4f)
    TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> ReadbackBuffer;

    // レンダースレッド → ゲームスレッド結果転送用。
    // キュー済みコマンドがサンプラー破棄後に本体メンバへ触れないよう共有状態に分離する。
    struct FReadbackSharedState
    {
        mutable FCriticalSection Lock;
        FVector4f RawData[12];
        FSamplingParams RawParams;
        bool bDataReady = false;
    };

    TSharedPtr<FReadbackSharedState, ESPMode::ThreadSafe> ReadbackState;
    FSamplingParams InFlightParams;

    // GPU Readback データを処理して Cached* に書き込む
    void ProcessReadbackData(const FVector4f* Data, const FSamplingParams& ResultParams);

    // トーンマッピング (Reinhard + 彩度ブースト)
    FLinearColor ApplyToneMapping(const FLinearColor& HDRColor, const FSamplingParams& ToneMappingParams) const;

    // CPU 同期フォールバック
    void ComputeAverageColor_CPUSync(UTextureRenderTargetCube* CubeRT);

    bool ShouldUseCPUPath() const { return bForceCPUSync || !bGPUPathAvailable; }
};
