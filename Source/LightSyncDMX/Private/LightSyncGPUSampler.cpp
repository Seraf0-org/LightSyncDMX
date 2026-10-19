// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncGPUSampler.h"
#include "LightSyncDMXModule.h"

#include "LightSyncOCIO.h"
#include "Engine/TextureRenderTargetCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ScreenPass.h"
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
#include "ProfilingDebugging/CpuProfilerTrace.h"

DECLARE_GPU_STAT_NAMED(LightSyncZones, TEXT("LightSync Zones"));

// ============================================================
// Compute Shader 宣言 (LightSyncAverageCS.usf)
// ============================================================

BEGIN_SHADER_PARAMETER_STRUCT(FLightSyncZoneShaderParameters, )
    SHADER_PARAMETER_RDG_TEXTURE_SRV(TextureCube, InputCube)
    SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, InputAtlas)
    SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
    SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, OutputBuffer)
    SHADER_PARAMETER(uint32, TextureSize)
    SHADER_PARAMETER(uint32, GroupsPerSide)
    SHADER_PARAMETER(uint32, NumZones)
    SHADER_PARAMETER(float, PartitionSharpness)
    SHADER_PARAMETER(FVector3f, LuminanceCoeffs)
    SHADER_PARAMETER(float, KeyRefineCos)
    SHADER_PARAMETER(uint32, InputTransfer)
    SHADER_PARAMETER(float, TransferGamma)
    SHADER_PARAMETER(float, DecodeGamma)
    SHADER_PARAMETER(float, PQScale)
    SHADER_PARAMETER_ARRAY(FVector4f, ZoneDirShape, [LightSyncZones::MaxZones])
    SHADER_PARAMETER_ARRAY(FVector4f, ZoneParams, [LightSyncZones::MaxZones])
END_SHADER_PARAMETER_STRUCT()

// 入力がキューブか、OCIO を掛けた 6 面アトラスか
class FLightSyncInputAtlasDim : SHADER_PERMUTATION_BOOL("INPUT_ATLAS");

#define LIGHTSYNC_DECLARE_ZONE_CS(ClassName)                                                         \
    class ClassName : public FGlobalShader                                                           \
    {                                                                                                \
        DECLARE_GLOBAL_SHADER(ClassName);                                                            \
        SHADER_USE_PARAMETER_STRUCT(ClassName, FGlobalShader);                                       \
        using FParameters = FLightSyncZoneShaderParameters;                                          \
        using FPermutationDomain = TShaderPermutationDomain<FLightSyncInputAtlasDim>;                \
                                                                                                     \
        static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)   \
        {                                                                                            \
            return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);              \
        }                                                                                            \
    };

LIGHTSYNC_DECLARE_ZONE_CS(FLightSyncPeakCS)
LIGHTSYNC_DECLARE_ZONE_CS(FLightSyncRefineCS)
LIGHTSYNC_DECLARE_ZONE_CS(FLightSyncRefineReduceCS)
LIGHTSYNC_DECLARE_ZONE_CS(FLightSyncZonesCS)
LIGHTSYNC_DECLARE_ZONE_CS(FLightSyncZonesReduceCS)
#undef LIGHTSYNC_DECLARE_ZONE_CS

/** キューブ → 6 面アトラス (OCIO を掛ける前に並べ替える) */
class FLightSyncCubeToAtlasCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FLightSyncCubeToAtlasCS);
    SHADER_USE_PARAMETER_STRUCT(FLightSyncCubeToAtlasCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE_SRV(TextureCube, InputCube)
        SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, AtlasOutput)
        SHADER_PARAMETER(uint32, TextureSize)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }
};

IMPLEMENT_GLOBAL_SHADER(FLightSyncCubeToAtlasCS, "/LightSyncDMX/Private/LightSyncAverageCS.usf", "MainCubeToAtlasCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLightSyncPeakCS, "/LightSyncDMX/Private/LightSyncAverageCS.usf", "MainPeakCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLightSyncRefineCS, "/LightSyncDMX/Private/LightSyncAverageCS.usf", "MainRefineCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLightSyncRefineReduceCS, "/LightSyncDMX/Private/LightSyncAverageCS.usf", "MainRefineReduceCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLightSyncZonesCS, "/LightSyncDMX/Private/LightSyncAverageCS.usf", "MainZonesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLightSyncZonesReduceCS, "/LightSyncDMX/Private/LightSyncAverageCS.usf", "MainZonesReduceCS", SF_Compute);

// ============================================================
// ヘルパー
// ============================================================

namespace
{
    /** 出力バッファの先頭 (ReadbackElements 個) → ゾーン結果 */
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
        const FVector4f Refine = Data[FLightSyncGPUSampler::RefineTotal];
        const FVector3f RefineDir(Refine.X, Refine.Y, Refine.Z);
        Result.KeyDirection = (Refine.W > 0.0f && !RefineDir.IsNearlyZero()) ? RefineDir.GetSafeNormal() : FVector3f(Peak.X, Peak.Y, Peak.Z);
        Result.KeyLuminance = FMath::Max(Peak.W, 0.0f);

        for (int32 Zone = 0; Zone < NumZones; ++Zone)
        {
            Result.Zones[Zone] = Data[FLightSyncGPUSampler::ZonesTotal + Zone];
        }
        return Result;
    }

    /**
     * キューブ → 6 面アトラス (一時) → OCIO → AtlasTex。
     * OCIO の入力はリニア (トーンカーブなし) なのでガンマ補正は 1 (nDisplay の外側ビューポートと同じ)
     */
    void AddOCIOAtlasPasses(FRDGBuilder& GraphBuilder, FRDGTextureSRVRef CubeSRV, FRDGTextureRef AtlasTex, uint32 TexSize,
                            const FOpenColorIORenderPassResources& Resources)
    {
        const FIntPoint Extent(static_cast<int32>(TexSize) * 6, static_cast<int32>(TexSize));
        FRDGTextureRef Linear = GraphBuilder.CreateTexture(
            FRDGTextureDesc::Create2D(Extent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
            TEXT("LS_LinearAtlas"));

        TShaderMapRef<FLightSyncCubeToAtlasCS> ToAtlasCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FLightSyncCubeToAtlasCS::FParameters* P = GraphBuilder.AllocParameters<FLightSyncCubeToAtlasCS::FParameters>();
        P->InputCube = CubeSRV;
        P->InputSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        P->AtlasOutput = GraphBuilder.CreateUAV(Linear);
        P->TextureSize = TexSize;
        FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightSync_CubeToAtlas"), ERDGPassFlags::Compute, ToAtlasCS, P,
                                     FIntVector(FMath::DivideAndRoundUp(static_cast<int32>(TexSize), 8),
                                                FMath::DivideAndRoundUp(static_cast<int32>(TexSize), 8), 6));

        FOpenColorIORendering::AddPass_RenderThread(
            GraphBuilder, FScreenPassViewInfo(), GMaxRHIFeatureLevel,
            FScreenPassTexture(Linear),
            FScreenPassRenderTarget(AtlasTex, ERenderTargetLoadAction::ENoAction),
            Resources, 1.0f);
    }

    void FillShaderParameters(FLightSyncZoneShaderParameters& P, const LightSyncZones::FSetup& Setup)
    {
        P.NumZones = static_cast<uint32>(FMath::Min(Setup.Zones.Num(), LightSyncZones::MaxZones));
        P.PartitionSharpness = Setup.PartitionSharpness;
        P.LuminanceCoeffs = Setup.LuminanceCoeffs;
        P.KeyRefineCos = Setup.KeyRefineCos;
        P.InputTransfer = static_cast<uint32>(Setup.InputTransfer);
        P.TransferGamma = Setup.TransferGamma;
        P.DecodeGamma = Setup.DecodeGamma;
        P.PQScale = 10000.0f / FMath::Max(Setup.PQReferenceWhiteNits, 0.01f);

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

void FLightSyncGPUSampler::DispatchSampling(UTextureRenderTargetCube* CubeRT, const FLightSyncOCIOPass* OCIO)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(LightSync_DispatchSampling);
    if (!bInitialized || !CubeRT)
    {
        return;
    }

    if (ShouldUseCPUPath())
    {
        ComputeCPU(CubeRT, OCIO);
        return;
    }

    // 前回のコピーが未取得の間は同じ ReadbackBuffer に新しいコピーを積まない
    if (bDispatched)
    {
        Poll();
        return;
    }

    FTextureResource* TexResource = CubeRT->GetResource();
    TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> ReadbackPtr = ReadbackBuffer;
    if (!TexResource || !TexResource->TextureRHI || !ReadbackPtr.IsValid())
    {
        ComputeCPU(CubeRT, OCIO);
        return;
    }

    // OCIO: アトラスのレンダーターゲットと、OCIO のシェーダー / LUT (レンダースレッドへコピー)
    const bool bUseOCIO = OCIO && OCIO->IsValid() && OCIO->Atlas->GetResource() && OCIO->Atlas->GetResource()->TextureRHI;
    FTextureRHIRef AtlasRHI = bUseOCIO ? OCIO->Atlas->GetResource()->TextureRHI : FTextureRHIRef();
    const FOpenColorIORenderPassResources OCIOResources = bUseOCIO ? OCIO->Resources : FOpenColorIORenderPassResources();

    FTextureRHIRef CubeTextureRHI = TexResource->TextureRHI;
    const uint32 TexSize = static_cast<uint32>(CubeRT->SizeX);
    const LightSyncZones::FSetup CapturedSetup = Setup;

    bDispatched = true;
    InFlightNumZones = FMath::Min(CapturedSetup.Zones.Num(), LightSyncZones::MaxZones);

    ENQUEUE_RENDER_COMMAND(LightSyncDispatchCS)(
        [CubeTextureRHI, TexSize, CapturedSetup, ReadbackPtr, bUseOCIO, AtlasRHI, OCIOResources](FRHICommandListImmediate& RHICmdList)
        {
            FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
            FLightSyncPeakCS::FPermutationDomain Permutation;
            Permutation.Set<FLightSyncInputAtlasDim>(bUseOCIO);
            TShaderMapRef<FLightSyncPeakCS> PeakCS(ShaderMap, Permutation);
            TShaderMapRef<FLightSyncRefineCS> RefineCS(ShaderMap, Permutation);
            TShaderMapRef<FLightSyncRefineReduceCS> RefineReduceCS(ShaderMap, Permutation);
            TShaderMapRef<FLightSyncZonesCS> ZonesCS(ShaderMap, Permutation);
            TShaderMapRef<FLightSyncZonesReduceCS> ZonesReduceCS(ShaderMap, Permutation);
            if (!PeakCS.IsValid() || !RefineCS.IsValid() || !RefineReduceCS.IsValid() || !ZonesCS.IsValid() || !ZonesReduceCS.IsValid())
            {
                return;
            }
            const int32 GroupsPerSide = GetGroupsPerSide(static_cast<int32>(TexSize));

            FRDGBuilder GraphBuilder(RHICmdList);
            {
                // GPU 統計のスコープは Execute() の前に閉じる (開いたままだと RDG のブレッドクラムが壊れる)
                RDG_EVENT_SCOPE_STAT(GraphBuilder, LightSyncZones, "LightSync Zones");
                RDG_GPU_STAT_SCOPE(GraphBuilder, LightSyncZones);

                FRDGTextureRef CubeTex = GraphBuilder.RegisterExternalTexture(
                    CreateRenderTarget(CubeTextureRHI, TEXT("LS_CubeRT")));
                FRDGTextureSRVRef CubeSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(CubeTex));

                // OCIO: キューブを 6 面アトラスに並べて OCIO を掛け、積分はアトラスから読む
                FRDGTextureSRVRef AtlasSRV = nullptr;
                if (bUseOCIO)
                {
                    FRDGTextureRef AtlasTex = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(AtlasRHI, TEXT("LS_OCIOAtlas")));
                    AddOCIOAtlasPasses(GraphBuilder, CubeSRV, AtlasTex, TexSize, OCIOResources);
                    AtlasSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(AtlasTex));
                }

                FRDGBufferRef OutputBuf = GraphBuilder.CreateBuffer(
                    FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumOutputElements),
                    TEXT("LS_ZoneOutput"));
                FRDGBufferUAVRef OutputUAV = GraphBuilder.CreateUAV(OutputBuf);

                auto MakeParams = [&]()
                {
                    FLightSyncZoneShaderParameters* P = GraphBuilder.AllocParameters<FLightSyncZoneShaderParameters>();
                    P->InputCube     = CubeSRV;
                    P->InputAtlas    = AtlasSRV;
                    P->InputSampler  = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                    P->OutputBuffer  = OutputUAV;
                    P->TextureSize   = TexSize;
                    P->GroupsPerSide = static_cast<uint32>(GroupsPerSide);
                    FillShaderParameters(*P, CapturedSetup);
                    return P;
                };

                // 各パスは前のパスの結果を同じバッファから読む (RDG がパス間に UAV バリアを入れる)
                const FIntVector PerFace(1, 1, 6);
                const FIntVector Grid(GroupsPerSide, GroupsPerSide, 6);
                const FIntVector Single(1, 1, 1);

                // Pass 1: 面ごとのピーク (Key ゾーン用)
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightSync_Peak"), ERDGPassFlags::Compute,
                                             PeakCS, MakeParams(), PerFace);

                // Pass 2: Key 方向の精密化
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightSync_Refine"), ERDGPassFlags::Compute,
                                             RefineCS, MakeParams(), Grid);
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightSync_RefineReduce"), ERDGPassFlags::Compute,
                                             RefineReduceCS, MakeParams(), Single);

                // Pass 3: ゾーン積分
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightSync_Zones %dx%dx6", GroupsPerSide, GroupsPerSide),
                                             ERDGPassFlags::Compute, ZonesCS, MakeParams(), Grid);
                FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("LightSync_ZonesReduce"), ERDGPassFlags::Compute,
                                             ZonesReduceCS, MakeParams(), Single);

                // CPU に読み戻すのは先頭の合計だけ
                AddEnqueueCopyPass(GraphBuilder, ReadbackPtr.Get(), OutputBuf, ReadbackElements * sizeof(FVector4f));
            }

            GraphBuilder.Execute();
        });
}

// ============================================================
// Poll
// ============================================================

void FLightSyncGPUSampler::Poll()
{
    if (!bInitialized || !bDispatched || ShouldUseCPUPath())
    {
        return;
    }

    TSharedPtr<FReadbackSharedState, ESPMode::ThreadSafe> SharedState = ReadbackState;
    TSharedPtr<FRHIGPUBufferReadback, ESPMode::ThreadSafe> ReadbackPtr = ReadbackBuffer;
    if (!SharedState.IsValid() || !ReadbackPtr.IsValid())
    {
        return;
    }

    {
        FScopeLock Lock(&SharedState->Lock);
        if (SharedState->bDataReady)
        {
            return;
        }
    }

    // IsReady/Lock/Unlock は必ずレンダースレッドから呼ぶ
    const int32 NumZones = InFlightNumZones;
    ENQUEUE_RENDER_COMMAND(LightSyncPollReadback)(
        [ReadbackPtr, SharedState, NumZones](FRHICommandListImmediate& /*RHICmdList*/)
        {
            if (ReadbackPtr->IsReady())
            {
                const FVector4f* Src = static_cast<const FVector4f*>(ReadbackPtr->Lock(ReadbackElements * sizeof(FVector4f)));
                if (Src)
                {
                    FScopeLock Lock(&SharedState->Lock);
                    FMemory::Memcpy(SharedState->RawData, Src, ReadbackElements * sizeof(FVector4f));
                    SharedState->NumZones = NumZones;
                    SharedState->bDataReady = true;
                }
                ReadbackPtr->Unlock();
            }
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

bool FLightSyncGPUSampler::ReadAtlasTexels(UTextureRenderTarget2D* AtlasRT, TArray<FLinearColor> (&OutFaces)[6], int32& OutSize)
{
    OutSize = 0;
    FRenderTarget* RenderTarget = AtlasRT ? AtlasRT->GameThread_GetRenderTargetResource() : nullptr;
    if (!RenderTarget || AtlasRT->SizeX != AtlasRT->SizeY * 6)
    {
        return false;
    }

    TArray<FFloat16Color> Pixels;
    const int32 Size = AtlasRT->SizeY;
    if (!RenderTarget->ReadFloat16Pixels(Pixels, FReadSurfaceDataFlags(RCM_MinMax)) || Pixels.Num() < Size * Size * 6)
    {
        return false;
    }

    for (int32 Face = 0; Face < 6; ++Face)
    {
        OutFaces[Face].SetNumUninitialized(Size * Size);
        for (int32 Y = 0; Y < Size; ++Y)
        {
            for (int32 X = 0; X < Size; ++X)
            {
                const FFloat16Color& C = Pixels[Y * Size * 6 + Face * Size + X];
                OutFaces[Face][Y * Size + X] = FLinearColor(C.R.GetFloat(), C.G.GetFloat(), C.B.GetFloat(), 1.0f);
            }
        }
    }
    OutSize = Size;
    return true;
}

void FLightSyncGPUSampler::ComputeCPU(UTextureRenderTargetCube* CubeRT, const FLightSyncOCIOPass* OCIO)
{
    TArray<FLinearColor> Faces[6];
    int32 Size = 0;

    // OCIO は GPU でしか掛けられないので、CPU 経路でもアトラスへの変換だけ GPU で行い、結果を読む
    if (OCIO && OCIO->IsValid() && CubeRT && CubeRT->GetResource() && OCIO->Atlas->GetResource())
    {
        FTextureRHIRef CubeRHI = CubeRT->GetResource()->TextureRHI;
        FTextureRHIRef AtlasRHI = OCIO->Atlas->GetResource()->TextureRHI;
        const uint32 TexSize = static_cast<uint32>(CubeRT->SizeX);
        const FOpenColorIORenderPassResources Resources = OCIO->Resources;
        ENQUEUE_RENDER_COMMAND(LightSyncOCIOAtlas)(
            [CubeRHI, AtlasRHI, TexSize, Resources](FRHICommandListImmediate& RHICmdList)
            {
                FRDGBuilder GraphBuilder(RHICmdList);
                FRDGTextureRef CubeTex = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(CubeRHI, TEXT("LS_CubeRT")));
                FRDGTextureRef AtlasTex = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(AtlasRHI, TEXT("LS_OCIOAtlas")));
                AddOCIOAtlasPasses(GraphBuilder, GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(CubeTex)), AtlasTex, TexSize, Resources);
                GraphBuilder.Execute();
            });
        if (!ReadAtlasTexels(OCIO->Atlas.Get(), Faces, Size))
        {
            return;
        }
    }
    else if (!ReadCubeTexels(CubeRT, Faces, Size))
    {
        return;
    }

    CPUResult = LightSyncZones::IntegrateCube(Faces, Size, Setup);
    bCPUResultPending = true;
}
