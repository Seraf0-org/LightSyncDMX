// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncGPUSampler.h"
#include "LightSyncDMXModule.h"

#include "Engine/TextureRenderTargetCube.h"
#include "TextureResource.h"

// RHI / RDG
#include "RHI.h"
#include "RHIStaticStates.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

// Global Shader
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ShaderCompilerCore.h"
#include "DataDrivenShaderPlatformInfo.h"

// ============================================================
// Compute Shader 宣言 (LightSyncAverageCS.usf の 2 エントリポイント)
// ============================================================

BEGIN_SHADER_PARAMETER_STRUCT(FLightSyncZoneShaderParameters, )
    SHADER_PARAMETER_RDG_TEXTURE_SRV(TextureCube, InputCube)
    SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, OutputBuffer)
    SHADER_PARAMETER(uint32, TextureSize)
    SHADER_PARAMETER(uint32, FaceIndex)
    SHADER_PARAMETER(uint32, NumZones)
    SHADER_PARAMETER(float, PartitionSharpness)
    SHADER_PARAMETER(FVector3f, LuminanceCoeffs)
    SHADER_PARAMETER(float, KeyRefineCos)
    SHADER_PARAMETER(uint32, InputTransfer)
    SHADER_PARAMETER_ARRAY(FVector4f, ZoneDirShape, [LightSyncZones::MaxZones])
    SHADER_PARAMETER_ARRAY(FVector4f, ZoneParams, [LightSyncZones::MaxZones])
END_SHADER_PARAMETER_STRUCT()

class FLightSyncPeakCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FLightSyncPeakCS);
    SHADER_USE_PARAMETER_STRUCT(FLightSyncPeakCS, FGlobalShader);
    using FParameters = FLightSyncZoneShaderParameters;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

class FLightSyncRefineCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FLightSyncRefineCS);
    SHADER_USE_PARAMETER_STRUCT(FLightSyncRefineCS, FGlobalShader);
    using FParameters = FLightSyncZoneShaderParameters;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

class FLightSyncZonesCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FLightSyncZonesCS);
    SHADER_USE_PARAMETER_STRUCT(FLightSyncZonesCS, FGlobalShader);
    using FParameters = FLightSyncZoneShaderParameters;

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FLightSyncPeakCS, "/LightSyncDMX/Private/LightSyncAverageCS.usf", "MainPeakCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLightSyncRefineCS, "/LightSyncDMX/Private/LightSyncAverageCS.usf", "MainRefineCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLightSyncZonesCS, "/LightSyncDMX/Private/LightSyncAverageCS.usf", "MainZonesCS", SF_Compute);

// ============================================================
// ヘルパー
// ============================================================

namespace
{
    /** 出力バッファ → ゾーン結果 (6 面を合計) */
    LightSyncZones::FResult DecodeOutput(const FVector4f* Data, int32 NumZones)
    {
        LightSyncZones::FResult Result;
        Result.Zones.SetNumZeroed(NumZones);

        FVector4f Peak = Data[0];
        for (int32 Face = 1; Face < 6; ++Face)
        {
            if (Data[Face].W > Peak.W)
            {
                Peak = Data[Face];
            }
        }
        FVector4f Refine(0.0f, 0.0f, 0.0f, 0.0f);
        for (int32 Face = 0; Face < 6; ++Face)
        {
            Refine += Data[FLightSyncGPUSampler::RefineBase + Face];
        }
        const FVector3f RefineDir(Refine.X, Refine.Y, Refine.Z);
        Result.KeyDirection = (Refine.W > 0.0f && !RefineDir.IsNearlyZero()) ? RefineDir.GetSafeNormal() : FVector3f(Peak.X, Peak.Y, Peak.Z);
        Result.KeyLuminance = FMath::Max(Peak.W, 0.0f);

        for (int32 Zone = 0; Zone < NumZones; ++Zone)
        {
            FVector4f Sum(0.0f, 0.0f, 0.0f, 0.0f);
            for (int32 Face = 0; Face < 6; ++Face)
            {
                Sum += Data[FLightSyncGPUSampler::ZonesBase + Face * LightSyncZones::MaxZones + Zone];
            }
            Result.Zones[Zone] = Sum;
        }
        return Result;
    }

    void FillShaderParameters(FLightSyncZoneShaderParameters& P, const LightSyncZones::FSetup& Setup)
    {
        P.NumZones = static_cast<uint32>(FMath::Min(Setup.Zones.Num(), LightSyncZones::MaxZones));
        P.PartitionSharpness = Setup.PartitionSharpness;
        P.LuminanceCoeffs = Setup.LuminanceCoeffs;
        P.KeyRefineCos = Setup.KeyRefineCos;
        P.InputTransfer = static_cast<uint32>(Setup.InputTransfer);

        for (int32 Index = 0; Index < LightSyncZones::MaxZones; ++Index)
        {
            if (Setup.Zones.IsValidIndex(Index))
            {
                const LightSyncZones::FZoneDesc& Zone = Setup.Zones[Index];
                P.ZoneDirShape[Index] = FVector4f(Zone.Direction, static_cast<float>(static_cast<uint8>(Zone.Shape)));
                P.ZoneParams[Index] = FVector4f(Zone.CosOuter, Zone.CosInner, Zone.bShared ? 1.0f : 0.0f, 0.0f);
            }
            else
            {
                P.ZoneDirShape[Index] = FVector4f(0.0f, 0.0f, 1.0f, 0.0f);
                P.ZoneParams[Index] = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
            }
        }
    }
}

// ============================================================
// FLightSyncGPUSampler 実装
// ============================================================

FLightSyncGPUSampler::FLightSyncGPUSampler()
{
}

FLightSyncGPUSampler::~FLightSyncGPUSampler()
{
    Release();
}

void FLightSyncGPUSampler::Initialize()
{
    if (bInitialized)
    {
        return;
    }

    // SM5 以上が使えるかチェック
    bGPUPathAvailable = (GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM5);

    if (bGPUPathAvailable)
    {
        ReadbackBuffer = MakeShared<FRHIGPUBufferReadback, ESPMode::ThreadSafe>(TEXT("LightSync_Readback"));
        UE_LOG(LogLightSyncDMX, Log, TEXT("LightSyncGPUSampler: GPU パス使用 (SM5+)"));
    }
    else
    {
        UE_LOG(LogLightSyncDMX, Warning,
               TEXT("LightSyncGPUSampler: フィーチャーレベルが SM5 未満のため CPU フォールバックを使用"));
    }

    ReadbackState = MakeShared<FReadbackSharedState, ESPMode::ThreadSafe>();
    FMemory::Memzero(ReadbackState->RawData, sizeof(ReadbackState->RawData));
    bDispatched  = false;
    bInitialized = true;
}

void FLightSyncGPUSampler::SetForceCPUSync(bool bInForceCPUSync)
{
    if (bForceCPUSync == bInForceCPUSync)
    {
        return;
    }

    bForceCPUSync = bInForceCPUSync;
    bDispatched = false;
    bCPUResultPending = false;

    if (ReadbackState)
    {
        FScopeLock Lock(&ReadbackState->Lock);
        ReadbackState->bDataReady = false;
    }
}

void FLightSyncGPUSampler::Release()
{
    if (!bInitialized)
    {
        return;
    }

    // キュー済みレンダーコマンドは TSharedPtr で readback の寿命を保持する。
    // UObject 破棄中に FlushRenderingCommands() で同期しない。
    ReadbackBuffer.Reset();
    ReadbackState.Reset();

    bInitialized      = false;
    bDispatched       = false;
    bCPUResultPending = false;

    UE_LOG(LogLightSyncDMX, Log, TEXT("LightSyncGPUSampler released"));
}

// ============================================================
// DispatchSampling
// ============================================================

void FLightSyncGPUSampler::DispatchSampling(UTextureRenderTargetCube* CubeRT)
{
    if (!bInitialized || !CubeRT)
    {
        return;
    }

    if (ShouldUseCPUPath())
    {
        ComputeCPU(CubeRT);
        return;
    }

    // --- 進行中のリードバックをレンダースレッドでポーリングする ---
    // IsReady/Lock/Unlock は必ずレンダースレッドから呼ぶ。
    if (bDispatched)
    {
        TSharedPtr<FReadbackSharedState, ESPMode::ThreadSafe> SharedState = ReadbackState;
        TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> ReadbackPtr = ReadbackBuffer;
        if (!SharedState.IsValid() || !ReadbackPtr.IsValid())
        {
            return;
        }

        bool bNeedsPoll = false;
        {
            FScopeLock Lock(&SharedState->Lock);
            bNeedsPoll = !SharedState->bDataReady;
        }

        if (bNeedsPoll)
        {
            const int32 NumZones = InFlightNumZones;
            ENQUEUE_RENDER_COMMAND(LightSyncPollReadback)(
                [ReadbackPtr, SharedState, NumZones](FRHICommandListImmediate& /*RHICmdList*/)
                {
                    if (ReadbackPtr->IsReady())
                    {
                        const FVector4f* Src = static_cast<const FVector4f*>(
                            ReadbackPtr->Lock(NumOutputElements * sizeof(FVector4f)));
                        if (Src)
                        {
                            FScopeLock Lock(&SharedState->Lock);
                            FMemory::Memcpy(SharedState->RawData, Src, NumOutputElements * sizeof(FVector4f));
                            SharedState->NumZones = NumZones;
                            SharedState->bDataReady = true;
                        }
                        ReadbackPtr->Unlock();
                    }
                });
        }

        // 前回のコピーが未取得の間は同じ ReadbackBuffer に新しいコピーを積まない
        return;
    }

    FTextureResource* TexResource = CubeRT->GetResource();
    TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> ReadbackPtr = ReadbackBuffer;
    if (!TexResource || !TexResource->TextureRHI || !ReadbackPtr.IsValid())
    {
        ComputeCPU(CubeRT);
        return;
    }

    FTextureRHIRef CubeTextureRHI = TexResource->TextureRHI;
    const uint32 TexSize = static_cast<uint32>(CubeRT->SizeX);
    const LightSyncZones::FSetup CapturedSetup = Setup;

    bDispatched = true;
    InFlightNumZones = FMath::Min(CapturedSetup.Zones.Num(), LightSyncZones::MaxZones);

    ENQUEUE_RENDER_COMMAND(LightSyncDispatchCS)(
        [CubeTextureRHI, TexSize, CapturedSetup, ReadbackPtr](FRHICommandListImmediate& RHICmdList)
        {
            FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
            TShaderMapRef<FLightSyncPeakCS> PeakCS(ShaderMap);
            TShaderMapRef<FLightSyncRefineCS> RefineCS(ShaderMap);
            TShaderMapRef<FLightSyncZonesCS> ZonesCS(ShaderMap);
            if (!PeakCS.IsValid() || !RefineCS.IsValid() || !ZonesCS.IsValid())
            {
                return;
            }

            FRDGBuilder GraphBuilder(RHICmdList);

            FRDGTextureRef CubeTex = GraphBuilder.RegisterExternalTexture(
                CreateRenderTarget(CubeTextureRHI, TEXT("LS_CubeRT")));
            FRDGTextureSRVRef CubeSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(CubeTex));

            FRDGBufferRef OutputBuf = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumOutputElements),
                TEXT("LS_ZoneOutput"));
            FRDGBufferUAVRef OutputUAV = GraphBuilder.CreateUAV(OutputBuf);

            auto MakeParams = [&](uint32 FaceIdx)
            {
                FLightSyncZoneShaderParameters* P = GraphBuilder.AllocParameters<FLightSyncZoneShaderParameters>();
                P->InputCube    = CubeSRV;
                P->InputSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                P->OutputBuffer = OutputUAV;
                P->TextureSize  = TexSize;
                P->FaceIndex    = FaceIdx;
                FillShaderParameters(*P, CapturedSetup);
                return P;
            };

            // Pass 1: 面ごとのピーク (Key ゾーン用)
            for (uint32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
            {
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightSync_Peak%u", FaceIdx),
                                             ERDGPassFlags::Compute, PeakCS, MakeParams(FaceIdx), FIntVector(1, 1, 1));
            }

            // Pass 2: Key 方向の精密化 (Pass 1 の結果を同じバッファから読む。RDG が UAV バリアを入れる)
            for (uint32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
            {
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightSync_Refine%u", FaceIdx),
                                             ERDGPassFlags::Compute, RefineCS, MakeParams(FaceIdx), FIntVector(1, 1, 1));
            }

            // Pass 3: ゾーン積分
            for (uint32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
            {
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightSync_Zones%u", FaceIdx),
                                             ERDGPassFlags::Compute, ZonesCS, MakeParams(FaceIdx), FIntVector(1, 1, 1));
            }

            AddEnqueueCopyPass(GraphBuilder, ReadbackPtr.Get(), OutputBuf, NumOutputElements * sizeof(FVector4f));

            GraphBuilder.Execute();
        });
}

// ============================================================
// TryGetNewResult
// ============================================================

bool FLightSyncGPUSampler::TryGetNewResult(LightSyncZones::FResult& OutResult)
{
    if (ShouldUseCPUPath())
    {
        if (bCPUResultPending)
        {
            OutResult = CPUResult;
            bCPUResultPending = false;
            return true;
        }
        return false;
    }

    TSharedPtr<FReadbackSharedState, ESPMode::ThreadSafe> SharedState = ReadbackState;
    if (!SharedState.IsValid())
    {
        return false;
    }

    FScopeLock Lock(&SharedState->Lock);
    if (!SharedState->bDataReady)
    {
        return false;
    }

    OutResult = DecodeOutput(SharedState->RawData, SharedState->NumZones);
    SharedState->bDataReady = false;
    bDispatched = false;
    return true;
}

// ============================================================
// CPU 経路
// ============================================================

bool FLightSyncGPUSampler::ReadCubeTexels(UTextureRenderTargetCube* CubeRT, TArray<FLinearColor> (&OutFaces)[6], int32& OutSize)
{
    OutSize = 0;
    if (!CubeRT || !CubeRT->GetResource())
    {
        return false;
    }

    FRenderTarget* RenderTarget = CubeRT->GameThread_GetRenderTargetResource();
    if (!RenderTarget)
    {
        return false;
    }

    const int32 Size = CubeRT->SizeX;
    for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
    {
        TArray<FFloat16Color> Pixels;
        FReadSurfaceDataFlags ReadFlags(RCM_MinMax);
        ReadFlags.SetCubeFace(static_cast<ECubeFace>(FaceIndex));
        if (!RenderTarget->ReadFloat16Pixels(Pixels, ReadFlags) || Pixels.Num() < Size * Size)
        {
            return false;
        }

        OutFaces[FaceIndex].SetNumUninitialized(Size * Size);
        for (int32 Index = 0; Index < Size * Size; ++Index)
        {
            OutFaces[FaceIndex][Index] = FLinearColor(Pixels[Index].R.GetFloat(), Pixels[Index].G.GetFloat(),
                                                      Pixels[Index].B.GetFloat(), 1.0f);
        }
    }

    OutSize = Size;
    return true;
}

void FLightSyncGPUSampler::ComputeCPU(UTextureRenderTargetCube* CubeRT)
{
    TArray<FLinearColor> Faces[6];
    int32 Size = 0;
    if (!ReadCubeTexels(CubeRT, Faces, Size))
    {
        return;
    }

    CPUResult = LightSyncZones::IntegrateCube(Faces, Size, Setup);
    bCPUResultPending = true;
}
