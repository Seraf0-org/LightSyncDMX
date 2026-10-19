// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncZones.h"

namespace LightSyncZones
{
    // シェーダー (LightSyncAverageCS.usf) のスレッド配置と一致させる
    static constexpr int32 ThreadsPerSide = 16;

    float DecodeTransfer(ELightSyncInputTransfer Transfer, float Value)
    {
        switch (Transfer)
        {
        case ELightSyncInputTransfer::SRGB:
            Value = FMath::Max(Value, 0.0f);
            return Value <= 0.04045f ? Value / 12.92f : FMath::Pow((Value + 0.055f) / 1.055f, 2.4f);
        case ELightSyncInputTransfer::Gamma22:
            return FMath::Pow(FMath::Max(Value, 0.0f), 2.2f);
        case ELightSyncInputTransfer::Gamma24:
            return FMath::Pow(FMath::Max(Value, 0.0f), 2.4f);
        default:
            return Value;
        }
    }

    FSetup BuildSetup(TConstArrayView<FLightSyncZone> Zones, const FQuat& ProbeRotation,
                      float PartitionSharpness, const FVector3f& LuminanceCoeffs, ELightSyncInputTransfer InputTransfer)
    {
        FSetup Setup;
        Setup.InputTransfer = InputTransfer;
        Setup.PartitionSharpness = FMath::Max(PartitionSharpness, 0.01f);
        Setup.LuminanceCoeffs = LuminanceCoeffs;

        for (const FLightSyncZone &Zone : Zones)
        {
            if (!Zone.bEnabled)
            {
                continue;
            }
            if (Setup.Zones.Num() >= MaxZones)
            {
                break;
            }

            FZoneDesc Desc;
            Desc.Shape = Zone.Shape;

            const FVector LocalDir = Zone.Direction.IsNearlyZero() ? FVector::UpVector : Zone.Direction.GetSafeNormal();
            Desc.Direction = FVector3f(ProbeRotation.RotateVector(LocalDir).GetSafeNormal());

            const float HalfSoft = 0.5f * FMath::Max(Zone.EdgeSoftness, 0.0f);
            const float OuterDeg = FMath::Clamp(Zone.ConeHalfAngle + HalfSoft, 0.0f, 180.0f);
            const float InnerDeg = FMath::Clamp(Zone.ConeHalfAngle - HalfSoft, 0.0f, 180.0f);
            Desc.CosOuter = FMath::Cos(FMath::DegreesToRadians(OuterDeg));
            Desc.CosInner = FMath::Cos(FMath::DegreesToRadians(InnerDeg));
            // smoothstep(outer, inner) が退化しないように最小幅を確保
            if (Desc.CosInner - Desc.CosOuter < 1e-4f)
            {
                Desc.CosInner = FMath::Min(Desc.CosOuter + 1e-4f, 1.0001f);
            }

            Desc.bShared = Zone.bShareDirections &&
                           (Zone.Shape == ELightSyncZoneShape::Irradiance ||
                            Zone.Shape == ELightSyncZoneShape::Ring ||
                            Zone.Shape == ELightSyncZoneShape::Cone);

            if (Desc.Shape == ELightSyncZoneShape::Key && Setup.KeyRefineCos > 1.0f)
            {
                Setup.KeyRefineCos = Desc.CosOuter;
            }

            Setup.Zones.Add(Desc);
        }

        return Setup;
    }

    FVector3f FaceUVToDirection(int32 Face, float U, float V)
    {
        const float S = U * 2.0f - 1.0f;
        const float T = V * 2.0f - 1.0f;

        switch (Face)
        {
        case 0:  return FVector3f( 1.0f, -T,   -S  );
        case 1:  return FVector3f(-1.0f, -T,    S  );
        case 2:  return FVector3f( S,     1.0f, T  );
        case 3:  return FVector3f( S,    -1.0f, -T );
        case 4:  return FVector3f( S,    -T,    1.0f);
        default: return FVector3f(-S,    -T,   -1.0f);
        }
    }

    float TexelSolidAngle(float U, float V, int32 Size)
    {
        const float S = U * 2.0f - 1.0f;
        const float T = V * 2.0f - 1.0f;
        const float D = 1.0f + S * S + T * T;
        return 4.0f / (static_cast<float>(Size) * static_cast<float>(Size) * D * FMath::Sqrt(D));
    }

    static float SmoothStep(float Edge0, float Edge1, float X)
    {
        const float T = FMath::Clamp((X - Edge0) / (Edge1 - Edge0), 0.0f, 1.0f);
        return T * T * (3.0f - 2.0f * T);
    }

    static float ConeMask(const FZoneDesc &Zone, const FVector3f &Dir, const FVector3f &Axis)
    {
        return SmoothStep(Zone.CosOuter, Zone.CosInner, FVector3f::DotProduct(Dir, Axis));
    }

    static float ExclusionKeep(const FSetup &Setup, const FVector3f &Dir)
    {
        float Keep = 1.0f;
        for (const FZoneDesc &Zone : Setup.Zones)
        {
            if (Zone.Shape == ELightSyncZoneShape::Exclude)
            {
                Keep *= 1.0f - ConeMask(Zone, Dir, Zone.Direction);
            }
        }
        return Keep;
    }

    static FVector3f LoadRadiance(const FSetup &Setup, const FLinearColor &C)
    {
        // シェーダーの DecodeTransfer → clamp(L, 0, 65504) と同じ。NaN は 0 に落とす
        auto Clean = [&Setup](float V)
        {
            V = DecodeTransfer(Setup.InputTransfer, V);
            if (Setup.TransferGamma != 1.0f)
            {
                V = FMath::Pow(FMath::Max(V, 0.0f), Setup.TransferGamma);
            }
            return FMath::IsNaN(V) ? 0.0f : FMath::Clamp(V, 0.0f, 65504.0f);
        };
        return FVector3f(Clean(C.R), Clean(C.G), Clean(C.B));
    }

    FResult IntegrateCube(const TArray<FLinearColor> (&FaceTexels)[6], int32 Size, const FSetup &Setup)
    {
        FResult Result;
        const int32 NumZones = Setup.Zones.Num();
        Result.Zones.SetNumZeroed(NumZones);

        if (Size <= 0)
        {
            return Result;
        }
        for (int32 Face = 0; Face < 6; ++Face)
        {
            if (FaceTexels[Face].Num() < Size * Size)
            {
                return Result;
            }
        }

        const int32 Per = FMath::Max(1, (Size + ThreadsPerSide - 1) / ThreadsPerSide);
        auto TexelUV = [Size](int32 X, int32 Y)
        {
            return FVector2f((X + 0.5f) / Size, (Y + 0.5f) / Size);
        };

        // --- Pass 1: ピーク探索 (ブロック平均輝度の最大) ---
        FVector4f Peak(0.0f, 0.0f, 1.0f, -1.0f);
        for (int32 Face = 0; Face < 6; ++Face)
        {
            FVector4f FacePeak(0.0f, 0.0f, 1.0f, -1.0f);
            for (int32 BlockY = 0; BlockY < ThreadsPerSide; ++BlockY)
            {
                for (int32 BlockX = 0; BlockX < ThreadsPerSide; ++BlockX)
                {
                    double LumSum = 0.0, OmegaSum = 0.0;
                    FVector3d DirSum = FVector3d::ZeroVector;

                    for (int32 Y = BlockY * Per; Y < FMath::Min(BlockY * Per + Per, Size); ++Y)
                    {
                        for (int32 X = BlockX * Per; X < FMath::Min(BlockX * Per + Per, Size); ++X)
                        {
                            const FVector2f UV = TexelUV(X, Y);
                            const FVector3f Dir = FaceUVToDirection(Face, UV.X, UV.Y).GetSafeNormal();
                            const float DOmega = TexelSolidAngle(UV.X, UV.Y, Size) * ExclusionKeep(Setup, Dir);
                            const float Lum = FVector3f::DotProduct(LoadRadiance(Setup, FaceTexels[Face][Y * Size + X]), Setup.LuminanceCoeffs);
                            LumSum += Lum * DOmega;
                            OmegaSum += DOmega;
                            DirSum += FVector3d(Dir) * (Lum * DOmega);
                        }
                    }

                    const float MeanLum = OmegaSum > 0.0 ? static_cast<float>(LumSum / OmegaSum) : 0.0f;
                    if (MeanLum > FacePeak.W)
                    {
                        const FVector3f PeakDir = DirSum.SizeSquared() > 0.0 ? FVector3f(DirSum.GetSafeNormal()) : FVector3f(0.0f, 0.0f, 1.0f);
                        FacePeak = FVector4f(PeakDir, MeanLum);
                    }
                }
            }
            if (Face == 0 || FacePeak.W > Peak.W)
            {
                Peak = FacePeak;
            }
        }

        const bool bHasKey = Peak.W > 0.0f;
        const FVector3f CoarseDir(Peak.X, Peak.Y, Peak.Z);

        // --- Pass 2: 粗いピークの周りのコーン内で輝度重心 (Key 方向の精密化) ---
        FVector4d Refine(0.0, 0.0, 0.0, 0.0);
        if (bHasKey && Setup.KeyRefineCos <= 1.0f)
        {
            for (int32 Face = 0; Face < 6; ++Face)
            {
                for (int32 Y = 0; Y < Size; ++Y)
                {
                    for (int32 X = 0; X < Size; ++X)
                    {
                        const FVector2f UV = TexelUV(X, Y);
                        const FVector3f Dir = FaceUVToDirection(Face, UV.X, UV.Y).GetSafeNormal();
                        if (FVector3f::DotProduct(Dir, CoarseDir) < Setup.KeyRefineCos)
                        {
                            continue;
                        }
                        const double W = FVector3f::DotProduct(LoadRadiance(Setup, FaceTexels[Face][Y * Size + X]), Setup.LuminanceCoeffs) *
                                         TexelSolidAngle(UV.X, UV.Y, Size) * ExclusionKeep(Setup, Dir);
                        Refine += FVector4d(Dir.X * W, Dir.Y * W, Dir.Z * W, W);
                    }
                }
            }
        }
        const FVector3d RefineDir(Refine.X, Refine.Y, Refine.Z);
        const FVector3f KeyDir = (Refine.W > 0.0 && !RefineDir.IsNearlyZero()) ? FVector3f(RefineDir.GetSafeNormal()) : CoarseDir;
        Result.KeyDirection = KeyDir;
        Result.KeyLuminance = FMath::Max(Peak.W, 0.0f);

        // --- Pass 3: ゾーン積分 ---
        TArray<FVector4d> Acc;
        Acc.SetNumZeroed(NumZones);
        TArray<float> Affinity;
        Affinity.SetNumZeroed(NumZones);

        for (int32 Face = 0; Face < 6; ++Face)
        {
            for (int32 Y = 0; Y < Size; ++Y)
            {
                for (int32 X = 0; X < Size; ++X)
                {
                    const FVector2f UV = TexelUV(X, Y);
                    const FVector3f Dir = FaceUVToDirection(Face, UV.X, UV.Y).GetSafeNormal();
                    const float DOmega = TexelSolidAngle(UV.X, UV.Y, Size);
                    const FVector3f L = LoadRadiance(Setup, FaceTexels[Face][Y * Size + X]);
                    const float Keep = ExclusionKeep(Setup, Dir);

                    float AffinitySum = 0.0f;
                    for (int32 Z = 0; Z < NumZones; ++Z)
                    {
                        const FZoneDesc &Zone = Setup.Zones[Z];
                        Affinity[Z] = 0.0f;
                        if (!Zone.bShared)
                        {
                            continue;
                        }
                        const float C = FVector3f::DotProduct(Dir, Zone.Direction);
                        float A = 0.0f;
                        switch (Zone.Shape)
                        {
                        case ELightSyncZoneShape::Irradiance: A = FMath::Max(0.0f, C); break;
                        case ELightSyncZoneShape::Ring:       A = FMath::Sqrt(FMath::Clamp(1.0f - C * C, 0.0f, 1.0f)); break;
                        case ELightSyncZoneShape::Cone:       A = ConeMask(Zone, Dir, Zone.Direction); break;
                        default: break;
                        }
                        Affinity[Z] = FMath::Pow(FMath::Max(A, 0.0f), Setup.PartitionSharpness);
                        AffinitySum += Affinity[Z];
                    }

                    for (int32 Z = 0; Z < NumZones; ++Z)
                    {
                        const FZoneDesc &Zone = Setup.Zones[Z];
                        if (Zone.Shape == ELightSyncZoneShape::Exclude)
                        {
                            continue;
                        }

                        float M;
                        if (Zone.bShared)
                        {
                            M = AffinitySum > 0.0f ? Affinity[Z] / AffinitySum : 0.0f;
                        }
                        else if (Zone.Shape == ELightSyncZoneShape::Cone)
                        {
                            M = ConeMask(Zone, Dir, Zone.Direction);
                        }
                        else if (Zone.Shape == ELightSyncZoneShape::Key)
                        {
                            M = bHasKey ? ConeMask(Zone, Dir, KeyDir) : 0.0f;
                        }
                        else
                        {
                            M = 1.0f;
                        }
                        M *= Keep;

                        float G;
                        if (Zone.Shape == ELightSyncZoneShape::Ring)
                        {
                            const float C = FVector3f::DotProduct(Dir, Zone.Direction);
                            G = FMath::Sqrt(FMath::Clamp(1.0f - C * C, 0.0f, 1.0f)) / (UE_PI * UE_PI);
                        }
                        else if (Zone.Shape == ELightSyncZoneShape::Sphere)
                        {
                            G = 1.0f / (4.0f * UE_PI);
                        }
                        else
                        {
                            const FVector3f Axis = (Zone.Shape == ELightSyncZoneShape::Key) ? KeyDir : Zone.Direction;
                            G = FMath::Max(0.0f, FVector3f::DotProduct(Dir, Axis)) / UE_PI;
                        }

                        const double W = static_cast<double>(M) * G * DOmega;
                        Acc[Z].X += L.X * W;
                        Acc[Z].Y += L.Y * W;
                        Acc[Z].Z += L.Z * W;
                        Acc[Z].W += static_cast<double>(M) * DOmega;
                    }
                }
            }
        }

        for (int32 Z = 0; Z < NumZones; ++Z)
        {
            Result.Zones[Z] = FVector4f(Acc[Z]);
        }
        return Result;
    }

    TArray<FLightSyncZone> MakeDefaultZones()
    {
        TArray<FLightSyncZone> Zones;

        FLightSyncZone Top;
        Top.Name = TEXT("Top");
        Top.Shape = ELightSyncZoneShape::Irradiance;
        Top.Direction = FVector::UpVector;
        Zones.Add(Top);

        FLightSyncZone Side;
        Side.Name = TEXT("Side");
        Side.Shape = ELightSyncZoneShape::Ring;
        Side.Direction = FVector::UpVector;
        Zones.Add(Side);

        FLightSyncZone Average;
        Average.Name = TEXT("Average");
        Average.Shape = ELightSyncZoneShape::Sphere;
        Zones.Add(Average);

        FLightSyncZone Key;
        Key.Name = TEXT("Key");
        Key.Shape = ELightSyncZoneShape::Key;
        Key.ConeHalfAngle = 20.0f;
        Zones.Add(Key);

        return Zones;
    }

    float RelativeChange(const TArray<FLinearColor>& A, const TArray<FLinearColor>& B, float Floor)
    {
        if (A.Num() != B.Num())
        {
            return TNumericLimits<float>::Max();
        }
        float MaxChange = 0.0f;
        for (int32 Index = 0; Index < A.Num(); ++Index)
        {
            const FLinearColor& X = A[Index];
            const FLinearColor& Y = B[Index];
            const float Scale = FMath::Max3(FMath::Max3(X.R, X.G, X.B), FMath::Max3(Y.R, Y.G, Y.B), Floor);
            const float Diff = FMath::Max3(FMath::Abs(X.R - Y.R), FMath::Abs(X.G - Y.G), FMath::Abs(X.B - Y.B));
            MaxChange = FMath::Max(MaxChange, Diff / FMath::Max(Scale, UE_SMALL_NUMBER));
        }
        return MaxChange;
    }
}
