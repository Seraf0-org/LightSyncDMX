// Copyright UE-Comp. All Rights Reserved.

#include "LightColorSamplerComponent.h"
#include "LightSyncDMXModule.h"

#include "Engine/TextureRenderTargetCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RHI.h"
#include "RenderUtils.h"
#include "RHIGPUReadback.h"

ULightColorSamplerComponent::ULightColorSamplerComponent()
{
    PrimaryComponentTick.bCanEverTick = false; // Tick不要、LightProbeActorから呼ばれる
}

void ULightColorSamplerComponent::BeginPlay()
{
    Super::BeginPlay();
    bIsFirstSample = true;
}

void ULightColorSamplerComponent::SampleFromRenderTarget(UTextureRenderTargetCube *InCubeRT)
{
    if (!bSamplingEnabled || !InCubeRT)
    {
        return;
    }

    // CubeMapの平均色を読み取る
    RawSampledColor = ReadAverageColorFromCube(InCubeRT);

    // スムージング適用
    if (bIsFirstSample)
    {
        SmoothedColor = RawSampledColor;
        bIsFirstSample = false;
    }
    else
    {
        SmoothedColor = SmoothColor(RawSampledColor, SmoothedColor, SmoothingAlpha);
    }
}

FLinearColor ULightColorSamplerComponent::ReadAverageColorFromCube(UTextureRenderTargetCube *CubeRT)
{
    if (!CubeRT || !CubeRT->GetResource())
    {
        return FLinearColor::Black;
    }

    const int32 Size = CubeRT->SizeX;

    // 面ごとの重み: +X, -X, +Y (上), -Y (下), +Z, -Z
    // CubeFace順序: PosX=0, NegX=1, PosY=2, NegY=3, PosZ=4, NegZ=5
    const float FaceWeights[6] = {
        SideFaceWeight,    // +X (右)
        SideFaceWeight,    // -X (左)
        TopFaceWeight,     // +Y (上/天井)
        BottomFaceWeight,  // -Y (下/床)
        SideFaceWeight,    // +Z (前)
        SideFaceWeight     // -Z (後)
    };

    // 加重平均用
    FLinearColor TotalColor = FLinearColor::Black;
    float TotalWeight = 0.0f;

    FRenderTarget *RenderTarget = CubeRT->GameThread_GetRenderTargetResource();
    if (!RenderTarget)
    {
        return FLinearColor::Black;
    }

    // CubeMapの各面 (6面) を読み取り
    for (int32 FaceIndex = 0; FaceIndex < 6; ++FaceIndex)
    {
        const float FaceWeight = FaceWeights[FaceIndex];
        if (FaceWeight <= KINDA_SMALL_NUMBER)
        {
            continue; // 重み0の面はスキップ
        }

        // UE 5.4: ReadFloat16Pixels で HDR データを直接取得
        TArray<FFloat16Color> Pixels;
        FReadSurfaceDataFlags ReadFlags(RCM_MinMax);
        ReadFlags.SetCubeFace(static_cast<ECubeFace>(FaceIndex));
        bool bSuccess = RenderTarget->ReadFloat16Pixels(Pixels, ReadFlags);

        if (!bSuccess || Pixels.Num() == 0)
        {
            continue;
        }

        // この面の平均色を計算
        FLinearColor FaceAvg = ReadAverageFromPixels(Pixels, Size, Size);

        TotalColor.R += FaceAvg.R * FaceWeight;
        TotalColor.G += FaceAvg.G * FaceWeight;
        TotalColor.B += FaceAvg.B * FaceWeight;
        TotalWeight += FaceWeight;
    }

    if (TotalWeight > KINDA_SMALL_NUMBER)
    {
        TotalColor.R /= TotalWeight;
        TotalColor.G /= TotalWeight;
        TotalColor.B /= TotalWeight;
    }

    TotalColor.A = 1.0f;
    return TotalColor;
}

FLinearColor ULightColorSamplerComponent::ReadAverageFromPixels(
    const TArray<FFloat16Color> &Pixels, int32 Width, int32 Height)
{
    if (Pixels.Num() == 0)
    {
        return FLinearColor::Black;
    }

    double AccumR = 0.0;
    double AccumG = 0.0;
    double AccumB = 0.0;
    int32 ValidCount = 0;

    // ダウンサンプリング: 全ピクセルではなくステップ刻みで読む
    const int32 StepX = FMath::Max(1, Width / DownsampleResolution);
    const int32 StepY = FMath::Max(1, Height / DownsampleResolution);

    for (int32 Y = 0; Y < Height; Y += StepY)
    {
        for (int32 X = 0; X < Width; X += StepX)
        {
            const int32 Index = Y * Width + X;
            if (Index >= Pixels.Num())
            {
                continue;
            }

            const FFloat16Color &Pixel = Pixels[Index];
            float R = Pixel.R.GetFloat();
            float G = Pixel.G.GetFloat();
            float B = Pixel.B.GetFloat();

            // HDRクランプ
            R = FMath::Min(R, HDRClampValue);
            G = FMath::Min(G, HDRClampValue);
            B = FMath::Min(B, HDRClampValue);

            // 暗部閾値
            float Luminance = 0.2126f * R + 0.7152f * G + 0.0722f * B;
            if (Luminance < DarkThreshold)
            {
                continue;
            }

            AccumR += R;
            AccumG += G;
            AccumB += B;
            ValidCount++;
        }
    }

    if (ValidCount == 0)
    {
        return FLinearColor::Black;
    }

    // HDR平均値を算出
    FLinearColor Result;
    Result.R = static_cast<float>(AccumR / ValidCount);
    Result.G = static_cast<float>(AccumG / ValidCount);
    Result.B = static_cast<float>(AccumB / ValidCount);
    Result.A = 1.0f;

    // === 輝度ベース Reinhard トーンマッピング (色相保持) ===
    // per-channel Reinhard は明るいチャンネルほど圧縮率が高く色相がシフトする。
    // 輝度を基準にトーンマッピングし、RGB を比例スケーリングすることで色相を保つ。
    const float Exposure = ToneMappingExposure;

    // Rec.709 輝度
    const float Lum = 0.2126f * Result.R + 0.7152f * Result.G + 0.0722f * Result.B;

    if (Lum > KINDA_SMALL_NUMBER)
    {
        // 輝度のみ Reinhard マッピング
        const float LumExposed = Lum * Exposure;
        const float MappedLum = LumExposed / (1.0f + LumExposed);

        // 全チャンネルを同一比率でスケール → 色相不変
        const float Scale = MappedLum / Lum;
        Result.R *= Scale;
        Result.G *= Scale;
        Result.B *= Scale;
    }
    else
    {
        // 暗すぎる場合はゼロに
        Result.R = 0.0f;
        Result.G = 0.0f;
        Result.B = 0.0f;
    }

    // トーンマッピング後のクランプ (浮動小数点の微小な超過対策)
    Result.R = FMath::Min(Result.R, 1.0f);
    Result.G = FMath::Min(Result.G, 1.0f);
    Result.B = FMath::Min(Result.B, 1.0f);

    return Result;
}

FLinearColor ULightColorSamplerComponent::SmoothColor(
    const FLinearColor &NewColor,
    const FLinearColor &PreviousColor,
    float Alpha)
{
    // EMA (指数移動平均) でちらつきを抑制
    FLinearColor Result;
    Result.R = FMath::Lerp(PreviousColor.R, NewColor.R, Alpha);
    Result.G = FMath::Lerp(PreviousColor.G, NewColor.G, Alpha);
    Result.B = FMath::Lerp(PreviousColor.B, NewColor.B, Alpha);
    Result.A = 1.0f;
    return Result;
}
