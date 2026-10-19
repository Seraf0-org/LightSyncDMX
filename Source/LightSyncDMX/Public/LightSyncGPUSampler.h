// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LightSyncZones.h"

class UTextureRenderTargetCube;
class FRHIGPUBufferReadback; // RHIGPUReadback.h をインクルードするのは .cpp 側

/**
 * FLightSyncGPUSampler
 *
 * キューブマップをゾーンごとに立体角で積分するサンプラー (LightSyncAverageCS.usf)。
 *
 * GPU Compute Shader (SM5+) で非同期に積分し、1〜2 フレーム後にゲームスレッドで結果を取得する。
 * GPU が使えない場合や CPU 指定時は、同じ式の CPU 版 (LightSyncZones::IntegrateCube) で同期計算する。
 *
 * 使い方:
 *   SetSetup() でゾーンを設定 → 毎サンプル DispatchSampling() → TryGetNewResult() で最新結果を取得。
 */
class LIGHTSYNCDMX_API FLightSyncGPUSampler
{
public:
    FLightSyncGPUSampler();
    ~FLightSyncGPUSampler();

    void Initialize();
    void Release();

    void SetSetup(const LightSyncZones::FSetup& InSetup) { Setup = InSetup; }
    void SetForceCPUSync(bool bInForceCPUSync);
    bool IsInitialized() const { return bInitialized; }

    /** キューブマップの積分を GPU (or CPU) にディスパッチ */
    void DispatchSampling(UTextureRenderTargetCube* CubeRT);

    /**
     * 新しい結果があれば OutResult に入れて true を返す。
     * OutResult.Zones の数はディスパッチ時点のゾーン数 (設定変更直後は現在の設定と違うことがある)。
     */
    bool TryGetNewResult(LightSyncZones::FResult& OutResult);

    /** CPU でキューブマップを読み出す (検証用。ゲームスレッドで同期読み出しするので重い) */
    static bool ReadCubeTexels(UTextureRenderTargetCube* CubeRT, TArray<FLinearColor> (&OutFaces)[6], int32& OutSize);

    /** GPU の出力バッファのレイアウト: ピーク 6 + 重心 6 + 面 6 × MaxZones */
    static constexpr int32 RefineBase = 6;
    static constexpr int32 ZonesBase = 12;
    static constexpr int32 NumOutputElements = ZonesBase + 6 * LightSyncZones::MaxZones;

private:
    LightSyncZones::FSetup Setup;

    bool bInitialized      = false;
    bool bDispatched       = false; // GPU ディスパッチが進行中 (ゲームスレッドのみ書き込み)
    bool bGPUPathAvailable = false;
    bool bForceCPUSync     = false;

    // CPU 経路の結果 (DispatchSampling で同期計算)
    bool bCPUResultPending = false;
    LightSyncZones::FResult CPUResult;

    // GPU Async Readback バッファ
    TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> ReadbackBuffer;

    // レンダースレッド → ゲームスレッド結果転送用。
    // キュー済みコマンドがサンプラー破棄後に本体メンバへ触れないよう共有状態に分離する。
    struct FReadbackSharedState
    {
        mutable FCriticalSection Lock;
        FVector4f RawData[NumOutputElements];
        int32 NumZones = 0;
        bool bDataReady = false;
    };

    TSharedPtr<FReadbackSharedState, ESPMode::ThreadSafe> ReadbackState;
    int32 InFlightNumZones = 0;

    void ComputeCPU(UTextureRenderTargetCube* CubeRT);

    bool ShouldUseCPUPath() const { return bForceCPUSync || !bGPUPathAvailable; }
};
