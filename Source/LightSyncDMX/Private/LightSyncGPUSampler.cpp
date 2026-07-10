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
// FLightSyncAverageCS — Global Compute Shader 宣言
// ============================================================

class FLightSyncAverageCS : public FGlobalShader
{
    DECLARE_GLOBAL_SHADER(FLightSyncAverageCS);
    SHADER_USE_PARAMETER_STRUCT(FLightSyncAverageCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE_SRV(TextureCube, InputCube)
        SHADER_PARAMETER_SAMPLER(SamplerState, InputSampler)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, OutputBuffer)
        SHADER_PARAMETER(uint32, TextureSize)
        SHADER_PARAMETER(uint32, FaceIndex)
        SHADER_PARAMETER(float,  LuminanceExponent)
        SHADER_PARAMETER(float,  DarkThreshold)
        SHADER_PARAMETER(float,  HDRClamp)
        SHADER_PARAMETER(float,  DirectLightThreshold)
        SHADER_PARAMETER(uint32, bFilterDirectLight)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(
        const FGlobalShaderPermutationParameters& Parameters,
        FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("LIGHTSYNC_THREADGROUP_SIZE"), 8);
    }
};

IMPLEMENT_GLOBAL_SHADER(
    FLightSyncAverageCS,
    "/LightSyncDMX/Private/LightSyncAverageCS.usf",
    "MainCS",
    SF_Compute);

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
        ReadbackBuffer = MakeUnique<FRHIGPUBufferReadback>(TEXT("LightSync_Readback"));
        UE_LOG(LogLightSyncDMX, Log, TEXT("LightSyncGPUSampler: GPU パス使用 (SM5+)"));
    }
    else
    {
        UE_LOG(LogLightSyncDMX, Warning,
               TEXT("LightSyncGPUSampler: フィーチャーレベルが SM5 未満のため CPU フォールバックを使用"));
    }

    FMemory::Memzero(ReadbackRawData, sizeof(ReadbackRawData));
    bDispatched        = false;
    bReadbackDataReady = false;
    bInitialized       = true;
}

void FLightSyncGPUSampler::Release()
{
    if (!bInitialized)
    {
        return;
    }

    // 進行中のレンダーコマンドがすべて完了するまで待つ
    FlushRenderingCommands();

    ReadbackBuffer.Reset();

    {
        FScopeLock Lock(&ReadbackResultLock);
        bReadbackDataReady = false;
    }

    bInitialized     = false;
    bDispatched      = false;
    bResultReady     = false;

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

    if (!bGPUPathAvailable)
    {
        // CPU 同期フォールバック: 結果はこの呼び出しで即座に Cached* に書き込まれる
        ComputeAverageColor_CPUSync(CubeRT);
        return;
    }

    // --- 前フレームのリードバックをレンダースレッドでポーリングする ---
    // IsReady/Lock/Unlock は必ずレンダースレッドから呼ぶ。
    // 結果を ReadbackRawData (FCriticalSection 保護) に書き込み、
    // ゲームスレッドは TryGetResult() で bReadbackDataReady フラグを確認する。
    if (bDispatched)
    {
        FRHIGPUBufferReadback* ReadbackPtr = ReadbackBuffer.Get();
        FCriticalSection*      LockPtr     = &ReadbackResultLock;
        FVector4f*             RawDataPtr  = ReadbackRawData;
        bool*                  ReadyFlagPtr = &bReadbackDataReady;

        ENQUEUE_RENDER_COMMAND(LightSyncPollReadback)(
            [ReadbackPtr, LockPtr, RawDataPtr, ReadyFlagPtr]
            (FRHICommandListImmediate& /*RHICmdList*/)
            {
                if (ReadbackPtr->IsReady())
                {
                    const FVector4f* Src = static_cast<const FVector4f*>(
                        ReadbackPtr->Lock(12 * sizeof(FVector4f)));
                    if (Src)
                    {
                        FScopeLock Lock(LockPtr);
                        FMemory::Memcpy(RawDataPtr, Src, 12 * sizeof(FVector4f));
                        *ReadyFlagPtr = true;
                    }
                    ReadbackPtr->Unlock();
                }
            });
    }

    // --- GPU テクスチャ参照をゲームスレッドで取得 ---
    FTextureResource* TexResource = CubeRT->GetResource();
    if (!TexResource || !TexResource->TextureRHI)
    {
        ComputeAverageColor_CPUSync(CubeRT);
        return;
    }

    FTextureRHIRef CubeTextureRHI = TexResource->TextureRHI;

    const uint32 TexSize = static_cast<uint32>(CubeRT->SizeX);
    const FSamplingParams CapturedParams = Params;
    FRHIGPUBufferReadback* ReadbackPtr   = ReadbackBuffer.Get();

    bDispatched = true;

    ENQUEUE_RENDER_COMMAND(LightSyncDispatchCS)(
        [CubeTextureRHI, TexSize, CapturedParams, ReadbackPtr]
        (FRHICommandListImmediate& RHICmdList)
        {
            // シェーダーが有効か確認
            TShaderMapRef<FLightSyncAverageCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
            if (!CS.IsValid())
            {
                return;
            }

            FRDGBuilder GraphBuilder(RHICmdList);

            // CubeMap テクスチャを RDG に登録
            FRDGTextureRef CubeTex = GraphBuilder.RegisterExternalTexture(
                CreateRenderTarget(CubeTextureRHI, TEXT("LS_CubeRT")));

            // TextureCube SRV を作成
            FRDGTextureSRVRef CubeSRV = GraphBuilder.CreateSRV(
                FRDGTextureSRVDesc::Create(CubeTex));

            // 出力用構造化バッファ (12 × float4)
            FRDGBufferRef OutputBuf = GraphBuilder.CreateBuffer(
                FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 12),
                TEXT("LS_OutputBuf"));

            FRDGBufferUAVRef OutputUAV = GraphBuilder.CreateUAV(OutputBuf);

            // 6 面分をディスパッチ
            for (uint32 FaceIdx = 0; FaceIdx < 6; ++FaceIdx)
            {
                FLightSyncAverageCS::FParameters* P =
                    GraphBuilder.AllocParameters<FLightSyncAverageCS::FParameters>();

                P->InputCube         = CubeSRV;
                P->InputSampler      = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
                P->OutputBuffer      = OutputUAV;
                P->TextureSize       = TexSize;
                P->FaceIndex         = FaceIdx;
                P->LuminanceExponent     = CapturedParams.LuminanceExponent;
                P->DarkThreshold         = CapturedParams.DarkThreshold;
                P->HDRClamp              = CapturedParams.HDRClamp;
                P->DirectLightThreshold  = CapturedParams.DirectLightThreshold;
                P->bFilterDirectLight    = CapturedParams.bFilterDirectLight ? 1u : 0u;

                FComputeShaderUtils::AddPass(
                    GraphBuilder,
                    RDG_EVENT_NAME("LightSync_Face%u", FaceIdx),
                    ERDGPassFlags::Compute,
                    CS,
                    P,
                    FIntVector(1, 1, 1));
            }

            // 結果をリードバックバッファにコピー (GPU → CPU ステージング)
            AddEnqueueCopyPass(GraphBuilder, ReadbackPtr, OutputBuf,
                               12 * sizeof(FVector4f));

            GraphBuilder.Execute();
        });
}

// ============================================================
// TryGetResult
// ============================================================

bool FLightSyncGPUSampler::TryGetResult(
    FLinearColor& OutAverageColor,
    FLinearColor& OutTopColor,
    FLinearColor& OutSideColor,
    FLinearColor& OutDominantColor)
{
    // CPU パス: DispatchSampling() で即時書き込み済み
    if (!bGPUPathAvailable)
    {
        if (bResultReady)
        {
            OutAverageColor  = CachedAverageColor;
            OutTopColor      = CachedTopColor;
            OutSideColor     = CachedSideColor;
            OutDominantColor = CachedDominantColor;
            return true;
        }
        return false;
    }

    // GPU パス: レンダースレッドがデータを書き込んでいれば取得する
    // (IsReady/Lock/Unlock はレンダースレッドの ENQUEUE_RENDER_COMMAND 内で行う)
    {
        FScopeLock Lock(&ReadbackResultLock);
        if (bReadbackDataReady)
        {
            ProcessReadbackData(ReadbackRawData);
            bReadbackDataReady = false;
            bDispatched        = false;
            bResultReady       = true;
        }
    }

    // キャッシュに有効な結果があれば返す (まだ未完了でも前フレームの値を維持)
    if (bResultReady)
    {
        OutAverageColor  = CachedAverageColor;
        OutTopColor      = CachedTopColor;
        OutSideColor     = CachedSideColor;
        OutDominantColor = CachedDominantColor;
        return true;
    }

    return false;
}

// ============================================================
// ProcessReadbackData — GPU 結果をポスト処理
// ============================================================

void FLightSyncGPUSampler::ProcessReadbackData(const FVector4f* Data)
{
    // 面設定ウェイト (UE5 は Z-up: +Z=天頂, -Z=床)
    const float FaceWeights[6] = {
        Params.SideFaceWeight,    // +X
        Params.SideFaceWeight,    // -X
        Params.SideFaceWeight,    // +Y
        Params.SideFaceWeight,    // -Y
        Params.TopFaceWeight,     // +Z (Top: 空/天井)
        Params.BottomFaceWeight,  // -Z (Bottom: 床)
    };

    // --- 全体平均 ---
    // Data[i] = (R*w, G*w, B*w, totalLumWeight)
    // 設定ウェイト FaceWeights[i] で面間の寄与率を調整する。
    // FaceColors[i] はすでに輝度加重済みなので de-weight してから再ウェイトする。
    {
        FVector4f Acc   = FVector4f(0,0,0,0);
        float     Total = 0.0f;
        for (int32 i = 0; i < 6; ++i)
        {
            float LumW  = Data[i].W; // 輝度加重の合計
            float FaceW = FaceWeights[i];
            if (LumW > KINDA_SMALL_NUMBER && FaceW > KINDA_SMALL_NUMBER)
            {
                // 輝度加重平均を de-weight して純粋な面平均を得る
                FVector4f FaceAvg = Data[i] / LumW;
                Acc.X += FaceAvg.X * FaceW;
                Acc.Y += FaceAvg.Y * FaceW;
                Acc.Z += FaceAvg.Z * FaceW;
                Total += FaceW;
            }
        }
        CachedAverageColor = (Total > KINDA_SMALL_NUMBER)
            ? FLinearColor(Acc.X / Total, Acc.Y / Total, Acc.Z / Total, 1.0f)
            : FLinearColor::Black;
    }

    // --- Top (面インデックス 4: +Z = UE5 の天頂方向) ---
    {
        float W = Data[4].W;
        CachedTopColor = (W > KINDA_SMALL_NUMBER)
            ? FLinearColor(Data[4].X / W, Data[4].Y / W, Data[4].Z / W, 1.0f)
            : FLinearColor::Black;
    }

    // --- Side (faces 0, 1, 2, 3: ±X, ±Y) ---
    {
        constexpr int32 SideFaceIdx[] = {0, 1, 2, 3};
        FVector4f Acc   = FVector4f(0,0,0,0);
        float     Total = 0.0f;
        for (int32 Idx : SideFaceIdx)
        {
            float LumW  = Data[Idx].W;
            float FaceW = FaceWeights[Idx];
            if (LumW > KINDA_SMALL_NUMBER && FaceW > KINDA_SMALL_NUMBER)
            {
                FVector4f FaceAvg = Data[Idx] / LumW;
                Acc.X += FaceAvg.X * FaceW;
                Acc.Y += FaceAvg.Y * FaceW;
                Acc.Z += FaceAvg.Z * FaceW;
                Total += FaceW;
            }
        }
        CachedSideColor = (Total > KINDA_SMALL_NUMBER)
            ? FLinearColor(Acc.X / Total, Acc.Y / Total, Acc.Z / Total, 1.0f)
            : FLinearColor::Black;
    }

    // --- Dominant (全 6 面で最大輝度のピクセル) ---
    // Data[6+i] = (R, G, B, luminance) of the brightest pixel on face i
    {
        int32 BestFace = -1;
        float BestLum  = -1.0f;
        for (int32 i = 0; i < 6; ++i)
        {
            float Lum = Data[6 + i].W;
            if (Lum > BestLum)
            {
                BestLum  = Lum;
                BestFace = i;
            }
        }
        CachedDominantColor = (BestFace >= 0 && BestLum > 0.0f)
            ? FLinearColor(Data[6 + BestFace].X, Data[6 + BestFace].Y, Data[6 + BestFace].Z, 1.0f)
            : FLinearColor::Black;
    }

    // トーンマッピングを適用
    CachedAverageColor  = ApplyToneMapping(CachedAverageColor);
    CachedTopColor      = ApplyToneMapping(CachedTopColor);
    CachedSideColor     = ApplyToneMapping(CachedSideColor);
    CachedDominantColor = ApplyToneMapping(CachedDominantColor);
}

// ============================================================
// ApplyToneMapping — Reinhard + 彩度ブースト
// ============================================================

FLinearColor FLightSyncGPUSampler::ApplyToneMapping(const FLinearColor& HDRColor) const
{
    FLinearColor Result = HDRColor;

    float Lum = 0.2126f * Result.R + 0.7152f * Result.G + 0.0722f * Result.B;

    if (Lum > KINDA_SMALL_NUMBER)
    {
        const float LumExposed = Lum * Params.ToneMappingExposure;
        const float MappedLum  = LumExposed / (1.0f + LumExposed); // Reinhard

        const float Scale = MappedLum / Lum;
        Result.R *= Scale;
        Result.G *= Scale;
        Result.B *= Scale;

        if (!FMath::IsNearlyEqual(Params.SaturationBoost, 1.0f))
        {
            const float NewLum = 0.2126f * Result.R + 0.7152f * Result.G + 0.0722f * Result.B;
            if (NewLum > KINDA_SMALL_NUMBER)
            {
                Result.R = NewLum + (Result.R - NewLum) * Params.SaturationBoost;
                Result.G = NewLum + (Result.G - NewLum) * Params.SaturationBoost;
                Result.B = NewLum + (Result.B - NewLum) * Params.SaturationBoost;
            }
        }
    }
    else
    {
        return FLinearColor::Black;
    }

    Result.R = FMath::Clamp(Result.R, 0.0f, 1.0f);
    Result.G = FMath::Clamp(Result.G, 0.0f, 1.0f);
    Result.B = FMath::Clamp(Result.B, 0.0f, 1.0f);
    Result.A = 1.0f;
    return Result;
}

// ============================================================
// ComputeAverageColor_CPUSync — CPU 同期フォールバック
// ============================================================

void FLightSyncGPUSampler::ComputeAverageColor_CPUSync(UTextureRenderTargetCube* CubeRT)
{
    if (!CubeRT || !CubeRT->GetResource())
    {
        return;
    }

    FRenderTarget* RenderTarget = CubeRT->GameThread_GetRenderTargetResource();
    if (!RenderTarget)
    {
        return;
    }

    const int32 Size = CubeRT->SizeX;

    // UE5 は Z-up: +Z=天頂, -Z=床
    const float FaceWeights[6] = {
        Params.SideFaceWeight,    // +X
        Params.SideFaceWeight,    // -X
        Params.SideFaceWeight,    // +Y
        Params.SideFaceWeight,    // -Y
        Params.TopFaceWeight,     // +Z (Top)
        Params.BottomFaceWeight,  // -Z (Bottom)
    };

    TArray<FLinearColor> FaceColors;
    TArray<float>        FaceColorWeights;
    FaceColors.SetNum(6);
    FaceColorWeights.SetNum(6);

    float         MaxLuminance      = 0.0f;
    FLinearColor  MaxLuminanceColor = FLinearColor::Black;

    for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
    {
        if (FaceWeights[FaceIndex] <= KINDA_SMALL_NUMBER)
        {
            FaceColors[FaceIndex]       = FLinearColor::Black;
            FaceColorWeights[FaceIndex] = 0.0f;
            continue;
        }

        TArray<FFloat16Color> Pixels;
        FReadSurfaceDataFlags ReadFlags(RCM_MinMax);
        ReadFlags.SetCubeFace(static_cast<ECubeFace>(FaceIndex));
        bool bSuccess = RenderTarget->ReadFloat16Pixels(Pixels, ReadFlags);

        if (!bSuccess || Pixels.Num() == 0)
        {
            FaceColors[FaceIndex]       = FLinearColor::Black;
            FaceColorWeights[FaceIndex] = 0.0f;
            continue;
        }

        double AccumR = 0.0, AccumG = 0.0, AccumB = 0.0;
        double TotalWeight = 0.0;
        float  FaceMaxLum  = 0.0f;
        FLinearColor FaceMaxColor = FLinearColor::Black;

        const int32 Step = FMath::Max(1, Size / 8);
        for (int32 Y = 0; Y < Size; Y += Step)
        {
            for (int32 X = 0; X < Size; X += Step)
            {
                const int32 Index = Y * Size + X;
                if (Index >= Pixels.Num()) continue;

                const FFloat16Color& Pixel = Pixels[Index];
                float R = FMath::Min(Pixel.R.GetFloat(), Params.HDRClamp);
                float G = FMath::Min(Pixel.G.GetFloat(), Params.HDRClamp);
                float B = FMath::Min(Pixel.B.GetFloat(), Params.HDRClamp);

                float Luminance = 0.2126f * R + 0.7152f * G + 0.0722f * B;
                if (Luminance < Params.DarkThreshold) continue;
                if (Params.bFilterDirectLight && Luminance > Params.DirectLightThreshold) continue;

                float Weight = FMath::Pow(Luminance, Params.LuminanceExponent);
                AccumR      += R * Weight;
                AccumG      += G * Weight;
                AccumB      += B * Weight;
                TotalWeight += Weight;

                if (Luminance > FaceMaxLum)
                {
                    FaceMaxLum   = Luminance;
                    FaceMaxColor = FLinearColor(R, G, B, 1.0f);
                }
            }
        }

        if (TotalWeight > KINDA_SMALL_NUMBER)
        {
            FaceColors[FaceIndex] = FLinearColor(
                static_cast<float>(AccumR / TotalWeight),
                static_cast<float>(AccumG / TotalWeight),
                static_cast<float>(AccumB / TotalWeight), 1.0f);
            FaceColorWeights[FaceIndex] = static_cast<float>(TotalWeight);
        }
        else
        {
            FaceColors[FaceIndex]       = FLinearColor::Black;
            FaceColorWeights[FaceIndex] = 0.0f;
        }

        if (FaceMaxLum > MaxLuminance)
        {
            MaxLuminance      = FaceMaxLum;
            MaxLuminanceColor = FaceMaxColor;
        }
    }

    // --- 全体平均 (設定ウェイトのみで面間加重) ---
    FLinearColor TotalColor = FLinearColor::Black;
    float        TotalW     = 0.0f;
    for (int32 i = 0; i < 6; ++i)
    {
        if (FaceWeights[i] > KINDA_SMALL_NUMBER && FaceColorWeights[i] > KINDA_SMALL_NUMBER)
        {
            TotalColor.R += FaceColors[i].R * FaceWeights[i];
            TotalColor.G += FaceColors[i].G * FaceWeights[i];
            TotalColor.B += FaceColors[i].B * FaceWeights[i];
            TotalW       += FaceWeights[i];
        }
    }
    CachedAverageColor = (TotalW > KINDA_SMALL_NUMBER)
        ? FLinearColor(TotalColor.R / TotalW, TotalColor.G / TotalW, TotalColor.B / TotalW, 1.0f)
        : FLinearColor::Black;

    // Top (+Z = 面 4, UE5 の天頂方向)
    CachedTopColor = FaceColors[4];

    // Side (faces 0,1,2,3: ±X, ±Y)
    FLinearColor SideAcc  = FLinearColor::Black;
    float        SideW    = 0.0f;
    constexpr int32 SideIdx[] = {0, 1, 2, 3};
    for (int32 Idx : SideIdx)
    {
        if (FaceColorWeights[Idx] > KINDA_SMALL_NUMBER)
        {
            SideAcc.R += FaceColors[Idx].R * FaceWeights[Idx];
            SideAcc.G += FaceColors[Idx].G * FaceWeights[Idx];
            SideAcc.B += FaceColors[Idx].B * FaceWeights[Idx];
            SideW     += FaceWeights[Idx];
        }
    }
    CachedSideColor = (SideW > KINDA_SMALL_NUMBER)
        ? FLinearColor(SideAcc.R / SideW, SideAcc.G / SideW, SideAcc.B / SideW, 1.0f)
        : FLinearColor::Black;

    CachedDominantColor = MaxLuminanceColor;

    CachedAverageColor  = ApplyToneMapping(CachedAverageColor);
    CachedTopColor      = ApplyToneMapping(CachedTopColor);
    CachedSideColor     = ApplyToneMapping(CachedSideColor);
    CachedDominantColor = ApplyToneMapping(CachedDominantColor);

    bResultReady = true;
}
