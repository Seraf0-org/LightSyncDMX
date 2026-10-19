// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "LightSyncZones.h"

class UTextureRenderTargetCube;
class UTextureRenderTarget2D;
class FRHIGPUBufferReadback; // RHIGPUReadback.h をインクルードするのは .cpp 側
struct FLightSyncOCIOPass;

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

    /**
     * キューブマップの積分を GPU (or CPU) にディスパッチ。
     * OCIO が有効なら、先に 6 面のアトラスへ並べて OCIO を掛け、そのアトラスを積分する
     */
    void DispatchSampling(UTextureRenderTargetCube* CubeRT, const FLightSyncOCIOPass* OCIO = nullptr);

    /** 前回の GPU 積分の結果待ちか (待っている間にキャプチャしても使われない) */
    bool IsBusy() const { return bInitialized && !ShouldUseCPUPath() && bDispatched; }

    /** 結果待ちならリードバックの完了を確認する (レンダースレッドで。毎フレーム呼んでよい) */
    void Poll();

    /**
     * 新しい結果があれば OutResult に入れて true を返す。
     * OutResult.Zones の数はディスパッチ時点のゾーン数 (設定変更直後は現在の設定と違うことがある)。
     */
    bool TryGetNewResult(LightSyncZones::FResult& OutResult);

    /** CPU でキューブマップを読み出す (検証用。ゲームスレッドで同期読み出しするので重い) */
    static bool ReadCubeTexels(UTextureRenderTargetCube* CubeRT, TArray<FLinearColor> (&OutFaces)[6], int32& OutSize);

    /** OCIO 後の 6 面アトラス (6N × N) を面ごとに読み出す (検証用) */
    static bool ReadAtlasTexels(UTextureRenderTarget2D* AtlasRT, TArray<FLinearColor> (&OutFaces)[6], int32& OutSize);

    /**
     * GPU の出力バッファのレイアウト (LightSyncAverageCS.usf と一致させる):
     *   [0..5] 面ごとのピーク, [6] 重心の合計, [8..15] ゾーンの合計 ← CPU に読み戻すのはここまで
     *   [16..] グループごとの部分和 (重心 MaxGroups 個, ゾーン MaxGroups × MaxZones 個)
     */
    static constexpr int32 RefineTotal = 6;
    static constexpr int32 ZonesTotal = 8;
    static constexpr int32 ReadbackElements = ZonesTotal + LightSyncZones::MaxZones;
    static constexpr int32 MaxGroupsPerSide = 8;
    static constexpr int32 MaxGroups = 6 * MaxGroupsPerSide * MaxGroupsPerSide;
    static constexpr int32 NumOutputElements = 16 + MaxGroups + MaxGroups * LightSyncZones::MaxZones;

    /** 1 面あたりのグループ数 (1 辺)。16×16 スレッドが 1 texel ずつ受け持つ程度まで分ける */
    static int32 GetGroupsPerSide(int32 TextureSize) { return FMath::Clamp((TextureSize + 15) / 16, 1, MaxGroupsPerSide); }

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
        FVector4f RawData[ReadbackElements];
        int32 NumZones = 0;
        bool bDataReady = false;
    };

    TSharedPtr<FReadbackSharedState, ESPMode::ThreadSafe> ReadbackState;
    int32 InFlightNumZones = 0;

    void ComputeCPU(UTextureRenderTargetCube* CubeRT, const FLightSyncOCIOPass* OCIO);

    bool ShouldUseCPUPath() const { return bForceCPUSync || !bGPUPathAvailable; }
};
