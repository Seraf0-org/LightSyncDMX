// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncColorMath.h"
#include "ColorManagement/ColorSpace.h"

#include <cfloat>
#include <cmath>

namespace LightSyncColor
{
    // =========================================================================
    // 3x3 行列
    // =========================================================================

    FMatrix3x3::FMatrix3x3()
    {
        for (int32 R = 0; R < 3; ++R)
        {
            for (int32 C = 0; C < 3; ++C)
            {
                M[R][C] = (R == C) ? 1.0 : 0.0;
            }
        }
    }

    FMatrix3x3 FMatrix3x3::FromColumns(const FVector3d& C0, const FVector3d& C1, const FVector3d& C2)
    {
        FMatrix3x3 Out;
        const FVector3d Cols[3] = { C0, C1, C2 };
        for (int32 C = 0; C < 3; ++C)
        {
            Out.M[0][C] = Cols[C].X;
            Out.M[1][C] = Cols[C].Y;
            Out.M[2][C] = Cols[C].Z;
        }
        return Out;
    }

    FVector3d FMatrix3x3::GetColumn(int32 Index) const
    {
        check(Index >= 0 && Index < 3);
        return FVector3d(M[0][Index], M[1][Index], M[2][Index]);
    }

    FVector3d FMatrix3x3::Transform(const FVector3d& V) const
    {
        return FVector3d(
            M[0][0] * V.X + M[0][1] * V.Y + M[0][2] * V.Z,
            M[1][0] * V.X + M[1][1] * V.Y + M[1][2] * V.Z,
            M[2][0] * V.X + M[2][1] * V.Y + M[2][2] * V.Z);
    }

    FMatrix3x3 FMatrix3x3::operator*(const FMatrix3x3& Other) const
    {
        FMatrix3x3 Out;
        for (int32 R = 0; R < 3; ++R)
        {
            for (int32 C = 0; C < 3; ++C)
            {
                Out.M[R][C] = M[R][0] * Other.M[0][C] + M[R][1] * Other.M[1][C] + M[R][2] * Other.M[2][C];
            }
        }
        return Out;
    }

    FMatrix3x3 FMatrix3x3::GetTransposed() const
    {
        FMatrix3x3 Out;
        for (int32 R = 0; R < 3; ++R)
        {
            for (int32 C = 0; C < 3; ++C)
            {
                Out.M[R][C] = M[C][R];
            }
        }
        return Out;
    }

    double FMatrix3x3::Determinant() const
    {
        return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1])
             - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
             + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    }

    bool FMatrix3x3::Inverse(FMatrix3x3& OutInverse) const
    {
        const double Det = Determinant();

        // スケールに依存しない特異判定: |det| を各行ノルムの積と比べる
        double RowNormProduct = 1.0;
        for (int32 R = 0; R < 3; ++R)
        {
            RowNormProduct *= FMath::Sqrt(M[R][0] * M[R][0] + M[R][1] * M[R][1] + M[R][2] * M[R][2]);
        }
        if (RowNormProduct <= 0.0 || FMath::Abs(Det) <= 1e-12 * RowNormProduct)
        {
            OutInverse = FMatrix3x3();
            return false;
        }

        const double InvDet = 1.0 / Det;
        OutInverse.M[0][0] =  (M[1][1] * M[2][2] - M[1][2] * M[2][1]) * InvDet;
        OutInverse.M[0][1] = -(M[0][1] * M[2][2] - M[0][2] * M[2][1]) * InvDet;
        OutInverse.M[0][2] =  (M[0][1] * M[1][2] - M[0][2] * M[1][1]) * InvDet;
        OutInverse.M[1][0] = -(M[1][0] * M[2][2] - M[1][2] * M[2][0]) * InvDet;
        OutInverse.M[1][1] =  (M[0][0] * M[2][2] - M[0][2] * M[2][0]) * InvDet;
        OutInverse.M[1][2] = -(M[0][0] * M[1][2] - M[0][2] * M[1][0]) * InvDet;
        OutInverse.M[2][0] =  (M[1][0] * M[2][1] - M[1][1] * M[2][0]) * InvDet;
        OutInverse.M[2][1] = -(M[0][0] * M[2][1] - M[0][1] * M[2][0]) * InvDet;
        OutInverse.M[2][2] =  (M[0][0] * M[1][1] - M[0][1] * M[1][0]) * InvDet;
        return true;
    }

    // =========================================================================
    // 1. 色空間
    // =========================================================================

    namespace
    {
        /** UE の FMatrix44d は行ベクトル規約（y = x · M） */
        FVector3d MulRowVector(const FMatrix44d& Mat, const FVector3d& V)
        {
            return FVector3d(
                V.X * Mat.M[0][0] + V.Y * Mat.M[1][0] + V.Z * Mat.M[2][0],
                V.X * Mat.M[0][1] + V.Y * Mat.M[1][1] + V.Z * Mat.M[2][1],
                V.X * Mat.M[0][2] + V.Y * Mat.M[1][2] + V.Z * Mat.M[2][2]);
        }
    }

    FVector3d WorkingRGBToXYZ(const FVector3d& RGB)
    {
        return MulRowVector(UE::Color::FColorSpace::GetWorking().GetRgbToXYZ(), RGB);
    }

    FVector3d XYZToWorkingRGB(const FVector3d& XYZ)
    {
        return MulRowVector(UE::Color::FColorSpace::GetWorking().GetXYZToRgb(), XYZ);
    }

    FVector3d XYZToxyY(const FVector3d& XYZ)
    {
        const double Sum = XYZ.X + XYZ.Y + XYZ.Z;
        if (FMath::Abs(Sum) <= 1e-20)
        {
            const FVector2d White = UE::Color::FColorSpace::GetWorking().GetWhiteChromaticity();
            return FVector3d(White.X, White.Y, 0.0);
        }
        return FVector3d(XYZ.X / Sum, XYZ.Y / Sum, XYZ.Y);
    }

    FVector3d xyYToXYZ(const FVector3d& xyY)
    {
        if (FMath::Abs(xyY.Y) <= 1e-20)
        {
            return FVector3d::ZeroVector;
        }
        const double Scale = xyY.Z / xyY.Y;
        return FVector3d(xyY.X * Scale, xyY.Z, (1.0 - xyY.X - xyY.Y) * Scale);
    }

    FVector2d XYZToUV1960(const FVector3d& XYZ)
    {
        const double Denom = XYZ.X + 15.0 * XYZ.Y + 3.0 * XYZ.Z;
        if (FMath::Abs(Denom) <= 1e-20)
        {
            return FVector2d::ZeroVector;
        }
        return FVector2d(4.0 * XYZ.X / Denom, 6.0 * XYZ.Y / Denom);
    }

    FVector2d UV1960Toxy(const FVector2d& UV)
    {
        const double Denom = 2.0 * UV.X - 8.0 * UV.Y + 4.0;
        if (FMath::Abs(Denom) <= 1e-20)
        {
            return FVector2d::ZeroVector;
        }
        return FVector2d(3.0 * UV.X / Denom, 2.0 * UV.Y / Denom);
    }

    FVector2d xyToUV1960(const FVector2d& xy)
    {
        const double Denom = -2.0 * xy.X + 12.0 * xy.Y + 3.0;
        if (FMath::Abs(Denom) <= 1e-20)
        {
            return FVector2d::ZeroVector;
        }
        return FVector2d(4.0 * xy.X / Denom, 6.0 * xy.Y / Denom);
    }

    // =========================================================================
    // 2. 色温度（CCT）と Duv
    // =========================================================================

    namespace
    {
        /**
         * CIE 1931 2° 標準観測者の等色関数 x̄, ȳ, z̄（CIE 015:2018 / CIE S 014-1 の 1nm 表を
         * 5nm ごとに抜き出したもの、360–830nm、95 点）。
         */
        constexpr int32 CMFCount = 95;
        constexpr double CMFStartNm = 360.0;
        constexpr double CMFStepNm = 5.0;
        constexpr double CIE1931CMF[CMFCount][3] =
        {
            { 1.299000e-04, 3.917000e-06, 6.061000e-04 }, // 360
            { 2.321000e-04, 6.965000e-06, 1.086000e-03 }, // 365
            { 4.149000e-04, 1.239000e-05, 1.946000e-03 }, // 370
            { 7.416000e-04, 2.202000e-05, 3.486000e-03 }, // 375
            { 1.368000e-03, 3.900000e-05, 6.450001e-03 }, // 380
            { 2.236000e-03, 6.400000e-05, 1.054999e-02 }, // 385
            { 4.243000e-03, 1.200000e-04, 2.005001e-02 }, // 390
            { 7.650000e-03, 2.170000e-04, 3.621000e-02 }, // 395
            { 1.431000e-02, 3.960000e-04, 6.785001e-02 }, // 400
            { 2.319000e-02, 6.400000e-04, 1.102000e-01 }, // 405
            { 4.351000e-02, 1.210000e-03, 2.074000e-01 }, // 410
            { 7.763000e-02, 2.180000e-03, 3.713000e-01 }, // 415
            { 1.343800e-01, 4.000000e-03, 6.456000e-01 }, // 420
            { 2.147700e-01, 7.300000e-03, 1.039050e+00 }, // 425
            { 2.839000e-01, 1.160000e-02, 1.385600e+00 }, // 430
            { 3.285000e-01, 1.684000e-02, 1.622960e+00 }, // 435
            { 3.482800e-01, 2.300000e-02, 1.747060e+00 }, // 440
            { 3.480600e-01, 2.980000e-02, 1.782600e+00 }, // 445
            { 3.362000e-01, 3.800000e-02, 1.772110e+00 }, // 450
            { 3.187000e-01, 4.800000e-02, 1.744100e+00 }, // 455
            { 2.908000e-01, 6.000000e-02, 1.669200e+00 }, // 460
            { 2.511000e-01, 7.390000e-02, 1.528100e+00 }, // 465
            { 1.953600e-01, 9.098000e-02, 1.287640e+00 }, // 470
            { 1.421000e-01, 1.126000e-01, 1.041900e+00 }, // 475
            { 9.564000e-02, 1.390200e-01, 8.129501e-01 }, // 480
            { 5.795001e-02, 1.693000e-01, 6.162000e-01 }, // 485
            { 3.201000e-02, 2.080200e-01, 4.651800e-01 }, // 490
            { 1.470000e-02, 2.586000e-01, 3.533000e-01 }, // 495
            { 4.900000e-03, 3.230000e-01, 2.720000e-01 }, // 500
            { 2.400000e-03, 4.073000e-01, 2.123000e-01 }, // 505
            { 9.300000e-03, 5.030000e-01, 1.582000e-01 }, // 510
            { 2.910000e-02, 6.082000e-01, 1.117000e-01 }, // 515
            { 6.327000e-02, 7.100000e-01, 7.824999e-02 }, // 520
            { 1.096000e-01, 7.932000e-01, 5.725001e-02 }, // 525
            { 1.655000e-01, 8.620000e-01, 4.216000e-02 }, // 530
            { 2.257499e-01, 9.148501e-01, 2.984000e-02 }, // 535
            { 2.904000e-01, 9.540000e-01, 2.030000e-02 }, // 540
            { 3.597000e-01, 9.803000e-01, 1.340000e-02 }, // 545
            { 4.334499e-01, 9.949501e-01, 8.749999e-03 }, // 550
            { 5.120501e-01, 1.000000e+00, 5.749999e-03 }, // 555
            { 5.945000e-01, 9.950000e-01, 3.900000e-03 }, // 560
            { 6.784000e-01, 9.786000e-01, 2.749999e-03 }, // 565
            { 7.621000e-01, 9.520000e-01, 2.100000e-03 }, // 570
            { 8.425000e-01, 9.154000e-01, 1.800000e-03 }, // 575
            { 9.163000e-01, 8.700000e-01, 1.650001e-03 }, // 580
            { 9.786000e-01, 8.163000e-01, 1.400000e-03 }, // 585
            { 1.026300e+00, 7.570000e-01, 1.100000e-03 }, // 590
            { 1.056700e+00, 6.949000e-01, 1.000000e-03 }, // 595
            { 1.062200e+00, 6.310000e-01, 8.000000e-04 }, // 600
            { 1.045600e+00, 5.668000e-01, 6.000000e-04 }, // 605
            { 1.002600e+00, 5.030000e-01, 3.400000e-04 }, // 610
            { 9.384000e-01, 4.412000e-01, 2.400000e-04 }, // 615
            { 8.544499e-01, 3.810000e-01, 1.900000e-04 }, // 620
            { 7.514000e-01, 3.210000e-01, 1.000000e-04 }, // 625
            { 6.424000e-01, 2.650000e-01, 4.999999e-05 }, // 630
            { 5.419000e-01, 2.170000e-01, 3.000000e-05 }, // 635
            { 4.479000e-01, 1.750000e-01, 2.000000e-05 }, // 640
            { 3.608000e-01, 1.382000e-01, 1.000000e-05 }, // 645
            { 2.835000e-01, 1.070000e-01, 0.000000e+00 }, // 650
            { 2.187000e-01, 8.160000e-02, 0.000000e+00 }, // 655
            { 1.649000e-01, 6.100000e-02, 0.000000e+00 }, // 660
            { 1.212000e-01, 4.458000e-02, 0.000000e+00 }, // 665
            { 8.740000e-02, 3.200000e-02, 0.000000e+00 }, // 670
            { 6.360000e-02, 2.320000e-02, 0.000000e+00 }, // 675
            { 4.677000e-02, 1.700000e-02, 0.000000e+00 }, // 680
            { 3.290000e-02, 1.192000e-02, 0.000000e+00 }, // 685
            { 2.270000e-02, 8.210000e-03, 0.000000e+00 }, // 690
            { 1.584000e-02, 5.723000e-03, 0.000000e+00 }, // 695
            { 1.135916e-02, 4.102000e-03, 0.000000e+00 }, // 700
            { 8.110916e-03, 2.929000e-03, 0.000000e+00 }, // 705
            { 5.790346e-03, 2.091000e-03, 0.000000e+00 }, // 710
            { 4.109457e-03, 1.484000e-03, 0.000000e+00 }, // 715
            { 2.899327e-03, 1.047000e-03, 0.000000e+00 }, // 720
            { 2.049190e-03, 7.400000e-04, 0.000000e+00 }, // 725
            { 1.439971e-03, 5.200000e-04, 0.000000e+00 }, // 730
            { 9.999493e-04, 3.611000e-04, 0.000000e+00 }, // 735
            { 6.900786e-04, 2.492000e-04, 0.000000e+00 }, // 740
            { 4.760213e-04, 1.719000e-04, 0.000000e+00 }, // 745
            { 3.323011e-04, 1.200000e-04, 0.000000e+00 }, // 750
            { 2.348261e-04, 8.480000e-05, 0.000000e+00 }, // 755
            { 1.661505e-04, 6.000000e-05, 0.000000e+00 }, // 760
            { 1.174130e-04, 4.240000e-05, 0.000000e+00 }, // 765
            { 8.307527e-05, 3.000000e-05, 0.000000e+00 }, // 770
            { 5.870652e-05, 2.120000e-05, 0.000000e+00 }, // 775
            { 4.150994e-05, 1.499000e-05, 0.000000e+00 }, // 780
            { 2.935326e-05, 1.060000e-05, 0.000000e+00 }, // 785
            { 2.067383e-05, 7.465700e-06, 0.000000e+00 }, // 790
            { 1.455977e-05, 5.257800e-06, 0.000000e+00 }, // 795
            { 1.025398e-05, 3.702900e-06, 0.000000e+00 }, // 800
            { 7.221456e-06, 2.607800e-06, 0.000000e+00 }, // 805
            { 5.085868e-06, 1.836600e-06, 0.000000e+00 }, // 810
            { 3.581652e-06, 1.293400e-06, 0.000000e+00 }, // 815
            { 2.522525e-06, 9.109300e-07, 0.000000e+00 }, // 820
            { 1.776509e-06, 6.415300e-07, 0.000000e+00 }, // 825
            { 1.251141e-06, 4.518100e-07, 0.000000e+00 }, // 830
        };

        /** Planck の第2放射定数 c2 = h·c/k [m·K]（CODATA 2018） */
        constexpr double PlanckC2 = 1.438776877e-2;

        /** 1% 刻みの Planck 表（Ohno 2013 の初期表）。初回呼び出し時に一度だけ作る */
        struct FPlanckTable
        {
            TArray<double> T;
            TArray<FVector2d> UV;

            FPlanckTable()
            {
                for (double Temp = MinCCT; Temp < MaxCCT; Temp *= 1.01)
                {
                    T.Add(Temp);
                    UV.Add(PlanckianUV(Temp));
                }
                T.Add(MaxCCT);
                UV.Add(PlanckianUV(MaxCCT));
            }
        };

        const FPlanckTable& GetPlanckTable()
        {
            static const FPlanckTable Table;
            return Table;
        }

        double DistUV(const FVector2d& A, const FVector2d& B)
        {
            return FMath::Sqrt((A.X - B.X) * (A.X - B.X) + (A.Y - B.Y) * (A.Y - B.Y));
        }
    }

    FVector2d PlanckianUV(double CCT)
    {
        const double T = FMath::Clamp(CCT, 100.0, 1.0e6);

        // 第1放射定数 c1 は u, v の比で消えるので省く
        double X = 0.0, Y = 0.0, Z = 0.0;
        for (int32 i = 0; i < CMFCount; ++i)
        {
            const double LambdaM = (CMFStartNm + CMFStepNm * i) * 1e-9;
            const double L5 = LambdaM * LambdaM * LambdaM * LambdaM * LambdaM;
            const double Spectral = 1.0 / (L5 * std::expm1(PlanckC2 / (LambdaM * T)));
            X += Spectral * CIE1931CMF[i][0];
            Y += Spectral * CIE1931CMF[i][1];
            Z += Spectral * CIE1931CMF[i][2];
        }
        return XYZToUV1960(FVector3d(X, Y, Z));
    }

    bool XYZToCCTDuv(const FVector3d& XYZ, double& OutCCT, double& OutDuv)
    {
        OutCCT = 0.0;
        OutDuv = 0.0;

        const double Denom = XYZ.X + 15.0 * XYZ.Y + 3.0 * XYZ.Z;
        if (!(Denom > 1e-20))
        {
            return false;
        }
        const FVector2d UV = XYZToUV1960(XYZ);

        // --- 1% 刻みの表で最小距離の点を探す ---
        const FPlanckTable& Table = GetPlanckTable();
        int32 Best = 0;
        double BestDist = TNumericLimits<double>::Max();
        for (int32 i = 0; i < Table.T.Num(); ++i)
        {
            const double D = DistUV(UV, Table.UV[i]);
            if (D < BestDist)
            {
                BestDist = D;
                Best = i;
            }
        }
        double Lo = Table.T[FMath::Max(Best - 1, 0)];
        double Hi = Table.T[FMath::Min(Best + 1, Table.T.Num() - 1)];

        // --- カスケード展開: 前後区間を 10 分割で張り直すのを繰り返す（1 回で幅 1/5） ---
        constexpr int32 Divisions = 10;
        constexpr int32 Iterations = 5;
        double Ts[Divisions + 1];
        FVector2d UVs[Divisions + 1];
        double Ds[Divisions + 1];
        int32 J = 0;
        for (int32 Iter = 0; Iter < Iterations; ++Iter)
        {
            for (int32 k = 0; k <= Divisions; ++k)
            {
                Ts[k] = Lo + (Hi - Lo) * k / Divisions;
                UVs[k] = PlanckianUV(Ts[k]);
                Ds[k] = DistUV(UV, UVs[k]);
            }
            J = 0;
            for (int32 k = 1; k <= Divisions; ++k)
            {
                if (Ds[k] < Ds[J])
                {
                    J = k;
                }
            }
            if (Iter + 1 < Iterations)
            {
                Lo = Ts[FMath::Max(J - 1, 0)];
                Hi = Ts[FMath::Min(J + 1, Divisions)];
            }
        }

        // レンジ端で最小になった場合は端の値を返す
        if (J == 0 || J == Divisions)
        {
            OutCCT = Ts[J];
            OutDuv = FMath::Sign(UV.Y - UVs[J].Y) * Ds[J];
            return true;
        }

        // --- 三角法（Ohno 2013 式 (6)–(9)） ---
        const double D0 = Ds[J - 1];
        const double D2 = Ds[J + 1];
        const double L = DistUV(UVs[J - 1], UVs[J + 1]);
        double TriCCT = Ts[J];
        double TriDuv = FMath::Sign(UV.Y - UVs[J].Y) * Ds[J];
        if (L > 0.0)
        {
            const double X = (D0 * D0 - D2 * D2 + L * L) / (2.0 * L);
            const double Frac = X / L;
            TriCCT = Ts[J - 1] + (Ts[J + 1] - Ts[J - 1]) * Frac;
            const double VX = UVs[J - 1].Y + (UVs[J + 1].Y - UVs[J - 1].Y) * Frac;
            TriDuv = FMath::Sign(UV.Y - VX) * FMath::Sqrt(FMath::Max(D0 * D0 - X * X, 0.0));
        }

        OutCCT = TriCCT;
        OutDuv = TriDuv;

        // --- |Duv| ≥ 0.002 では放物線法（式 (10)–(14)）。数値安定のため Ts[J] を原点にとる ---
        if (FMath::Abs(TriDuv) >= 0.002)
        {
            const double T0 = Ts[J - 1] - Ts[J];
            const double T2 = Ts[J + 1] - Ts[J];
            const double C = Ds[J];
            const double S0 = (D0 - C) / T0;
            const double S2 = (D2 - C) / T2;
            const double A = (S0 - S2) / (T0 - T2);
            const double B = S0 - A * T0;
            if (A > 0.0)
            {
                const double TMin = -B / (2.0 * A);
                const double DMin = A * TMin * TMin + B * TMin + C;
                OutCCT = Ts[J] + TMin;
                OutDuv = FMath::Sign(TriDuv) * DMin;
            }
        }
        return true;
    }

    FVector2d CCTDuvToxy(double CCT, double Duv)
    {
        const double T0 = FMath::Clamp(CCT, MinCCT, MaxCCT);
        constexpr double DeltaT = 0.01;

        // Ohno 2013: du = u(T0) − u(T0+ΔT)、dv = v(T0) − v(T0+ΔT)
        const FVector2d UV0 = PlanckianUV(T0);
        FVector2d Diff = UV0 - PlanckianUV(T0 + DeltaT);
        if (T0 + DeltaT > MaxCCT)
        {
            // 上端では後退差分（向きは同じになるよう符号を合わせる）
            Diff = PlanckianUV(T0 - DeltaT) - UV0;
        }
        const double Len = Diff.Size();
        if (Len <= 0.0)
        {
            return UV1960Toxy(UV0);
        }
        const double SinT = Diff.Y / Len;
        const double CosT = Diff.X / Len;
        const FVector2d UV(UV0.X - Duv * SinT, UV0.Y + Duv * CosT);
        return UV1960Toxy(UV);
    }

    // =========================================================================
    // 3. 灯体ソルバ
    // =========================================================================

    namespace
    {
        constexpr int32 MaxNNLSRows = 32;
        constexpr int32 MaxNNLSCols = 16;

        /**
         * 最小二乗 min |A_P · s − b|（P = 列の部分集合）を Householder QR で解く。
         * ランクが落ちた列の解は 0 にする。
         */
        void SolveSubsetLeastSquares(const TArray<double>& A, int32 Rows, int32 Cols, const TArray<double>& B,
                                     const int32* Subset, int32 SubsetNum, double* OutS)
        {
            double Q[MaxNNLSRows][MaxNNLSCols];
            double Rhs[MaxNNLSRows];
            for (int32 r = 0; r < Rows; ++r)
            {
                for (int32 k = 0; k < SubsetNum; ++k)
                {
                    Q[r][k] = A[r * Cols + Subset[k]];
                }
                Rhs[r] = B[r];
            }

            // 列ノルムの最大値（ランク判定用）
            double MaxColNorm = 0.0;
            for (int32 k = 0; k < SubsetNum; ++k)
            {
                double N2 = 0.0;
                for (int32 r = 0; r < Rows; ++r)
                {
                    N2 += Q[r][k] * Q[r][k];
                }
                MaxColNorm = FMath::Max(MaxColNorm, FMath::Sqrt(N2));
            }
            const double RankTol = 1e-13 * FMath::Max(MaxColNorm, 1e-300);

            const int32 Steps = FMath::Min(SubsetNum, Rows);
            for (int32 k = 0; k < Steps; ++k)
            {
                double Norm2 = 0.0;
                for (int32 r = k; r < Rows; ++r)
                {
                    Norm2 += Q[r][k] * Q[r][k];
                }
                const double Norm = FMath::Sqrt(Norm2);
                if (Norm <= RankTol)
                {
                    continue;
                }
                const double Alpha = (Q[k][k] > 0.0) ? -Norm : Norm;
                double V[MaxNNLSRows];
                double VNorm2 = 0.0;
                for (int32 r = k; r < Rows; ++r)
                {
                    V[r] = Q[r][k];
                }
                V[k] -= Alpha;
                for (int32 r = k; r < Rows; ++r)
                {
                    VNorm2 += V[r] * V[r];
                }
                if (VNorm2 <= 0.0)
                {
                    continue;
                }
                for (int32 c = k; c < SubsetNum; ++c)
                {
                    double Dot = 0.0;
                    for (int32 r = k; r < Rows; ++r)
                    {
                        Dot += V[r] * Q[r][c];
                    }
                    const double F = 2.0 * Dot / VNorm2;
                    for (int32 r = k; r < Rows; ++r)
                    {
                        Q[r][c] -= F * V[r];
                    }
                }
                double Dot = 0.0;
                for (int32 r = k; r < Rows; ++r)
                {
                    Dot += V[r] * Rhs[r];
                }
                const double F = 2.0 * Dot / VNorm2;
                for (int32 r = k; r < Rows; ++r)
                {
                    Rhs[r] -= F * V[r];
                }
            }

            // 後退代入
            for (int32 k = SubsetNum - 1; k >= 0; --k)
            {
                if (k >= Rows || FMath::Abs(Q[k][k]) <= RankTol)
                {
                    OutS[k] = 0.0;
                    continue;
                }
                double Sum = Rhs[k];
                for (int32 c = k + 1; c < SubsetNum; ++c)
                {
                    Sum -= Q[k][c] * OutS[c];
                }
                OutS[k] = Sum / Q[k][k];
            }
        }

        double ResidualNorm(const TArray<double>& A, int32 Rows, int32 Cols, const TArray<double>& B, const TArray<double>& X)
        {
            double Sum = 0.0;
            for (int32 r = 0; r < Rows; ++r)
            {
                double Ax = 0.0;
                for (int32 c = 0; c < Cols; ++c)
                {
                    Ax += A[r * Cols + c] * X[c];
                }
                Sum += (Ax - B[r]) * (Ax - B[r]);
            }
            return FMath::Sqrt(Sum);
        }
    }

    double SolveNNLS(const TArray<double>& A, int32 Rows, int32 Cols, const TArray<double>& B, TArray<double>& OutX)
    {
        OutX.Init(0.0, FMath::Max(Cols, 0));
        if (Rows <= 0 || Cols <= 0 || Rows > MaxNNLSRows || Cols > MaxNNLSCols
            || A.Num() < Rows * Cols || B.Num() < Rows)
        {
            double Sum = 0.0;
            for (int32 r = 0; r < FMath::Min(Rows, B.Num()); ++r)
            {
                Sum += B[r] * B[r];
            }
            return FMath::Sqrt(Sum);
        }

        // 許容誤差（MATLAB lsqnonneg と同じ考え方: 10·eps·|A|_1·max(m,n)）
        double NormA1 = 0.0;
        for (int32 c = 0; c < Cols; ++c)
        {
            double ColSum = 0.0;
            for (int32 r = 0; r < Rows; ++r)
            {
                ColSum += FMath::Abs(A[r * Cols + c]);
            }
            NormA1 = FMath::Max(NormA1, ColSum);
        }
        double NormB = 0.0;
        for (int32 r = 0; r < Rows; ++r)
        {
            NormB = FMath::Max(NormB, FMath::Abs(B[r]));
        }
        const double Tol = 10.0 * DBL_EPSILON * NormA1 * FMath::Max(Rows, Cols) * FMath::Max(NormB, 1.0);

        bool bPassive[MaxNNLSCols] = {};
        bool bBlocked[MaxNNLSCols] = {};
        double X[MaxNNLSCols] = {};
        double W[MaxNNLSCols];
        double S[MaxNNLSCols];
        int32 Subset[MaxNNLSCols];

        auto ComputeGradient = [&]()
        {
            double Res[MaxNNLSRows];
            for (int32 r = 0; r < Rows; ++r)
            {
                double Ax = 0.0;
                for (int32 c = 0; c < Cols; ++c)
                {
                    Ax += A[r * Cols + c] * X[c];
                }
                Res[r] = B[r] - Ax;
            }
            for (int32 c = 0; c < Cols; ++c)
            {
                double Dot = 0.0;
                for (int32 r = 0; r < Rows; ++r)
                {
                    Dot += A[r * Cols + c] * Res[r];
                }
                W[c] = Dot;
            }
        };

        const int32 MaxOuter = 3 * Cols + 10;
        ComputeGradient();
        for (int32 Outer = 0; Outer < MaxOuter; ++Outer)
        {
            // 能動集合（x=0）の中で勾配が最大の列を選ぶ
            int32 Pick = INDEX_NONE;
            double PickW = Tol;
            for (int32 c = 0; c < Cols; ++c)
            {
                if (!bPassive[c] && !bBlocked[c] && W[c] > PickW)
                {
                    PickW = W[c];
                    Pick = c;
                }
            }
            if (Pick == INDEX_NONE)
            {
                break;
            }
            bPassive[Pick] = true;

            // 内側ループ: 受動集合で最小二乗を解き、負になった変数を落とす
            for (int32 Inner = 0; Inner < 3 * Cols + 10; ++Inner)
            {
                // 丸め誤差で、加えたばかりの変数が正にならない場合は今回は候補から外す（無限ループ防止）
                const bool bFirstInner = (Inner == 0);
                int32 SubsetNum = 0;
                for (int32 c = 0; c < Cols; ++c)
                {
                    if (bPassive[c])
                    {
                        Subset[SubsetNum++] = c;
                    }
                }
                double SubS[MaxNNLSCols];
                SolveSubsetLeastSquares(A, Rows, Cols, B, Subset, SubsetNum, SubS);
                for (int32 c = 0; c < Cols; ++c)
                {
                    S[c] = 0.0;
                }
                for (int32 k = 0; k < SubsetNum; ++k)
                {
                    S[Subset[k]] = SubS[k];
                }

                bool bAllPositive = true;
                for (int32 k = 0; k < SubsetNum; ++k)
                {
                    if (S[Subset[k]] <= 0.0)
                    {
                        bAllPositive = false;
                        break;
                    }
                }
                if (bAllPositive)
                {
                    for (int32 c = 0; c < Cols; ++c)
                    {
                        X[c] = S[c];
                        bBlocked[c] = false;
                    }
                    break;
                }
                if (bFirstInner && S[Pick] <= 0.0)
                {
                    bPassive[Pick] = false;
                    bBlocked[Pick] = true;
                    break;
                }

                // x から s へ向かう線分上で、最初に 0 に当たる位置まで進む
                double Alpha = 1.0;
                for (int32 k = 0; k < SubsetNum; ++k)
                {
                    const int32 c = Subset[k];
                    if (S[c] <= 0.0)
                    {
                        const double Den = X[c] - S[c];
                        if (Den > 0.0)
                        {
                            Alpha = FMath::Min(Alpha, X[c] / Den);
                        }
                        else
                        {
                            Alpha = 0.0;
                        }
                    }
                }
                for (int32 c = 0; c < Cols; ++c)
                {
                    X[c] += Alpha * (S[c] - X[c]);
                }
                for (int32 k = 0; k < SubsetNum; ++k)
                {
                    const int32 c = Subset[k];
                    if (X[c] <= Tol)
                    {
                        X[c] = 0.0;
                        bPassive[c] = false;
                    }
                }
            }

            ComputeGradient();
        }

        for (int32 c = 0; c < Cols; ++c)
        {
            OutX[c] = FMath::Max(X[c], 0.0);
        }
        return ResidualNorm(A, Rows, Cols, B, OutX);
    }

    FVector3d FEmitterSet::GetWhiteResponse() const
    {
        if (Responses.IsValidIndex(PreferredWhiteIndex))
        {
            return Responses[PreferredWhiteIndex];
        }
        FVector3d Sum = FVector3d::ZeroVector;
        for (const FVector3d& R : Responses)
        {
            Sum += R;
        }
        return Sum;
    }

    namespace
    {
        /** 目標が「再現できる」とみなす相対残差 */
        constexpr double FeasibleRelTol = 1e-7;

        /** 色の一致を正則化項より優先させるための重み（列ノルム基準） */
        constexpr double ExactnessWeight = 1e4;

        struct FSolverContext
        {
            const FEmitterSet& Emitters;
            int32 N = 0;
            TArray<double> A;          // 3 x N（行優先）
            TArray<double> Penalty;    // Tikhonov の係数 c_i
            double ColumnScale = 1.0;  // 列ノルムの代表値

            explicit FSolverContext(const FEmitterSet& InEmitters)
                : Emitters(InEmitters)
            {
                N = FMath::Min(Emitters.Responses.Num(), MaxNNLSCols);
                A.SetNumZeroed(3 * N);
                double SumNorm = 0.0;
                for (int32 c = 0; c < N; ++c)
                {
                    const FVector3d& R = Emitters.Responses[c];
                    A[0 * N + c] = R.X;
                    A[1 * N + c] = R.Y;
                    A[2 * N + c] = R.Z;
                    SumNorm += R.Size();
                }
                ColumnScale = (N > 0 && SumNorm > 0.0) ? SumNorm / N : 1.0;

                const bool bHasWhite = Emitters.PreferredWhiteIndex >= 0 && Emitters.PreferredWhiteIndex < N;
                const double WhiteCost = FMath::Clamp(1.0 - Emitters.WhitePreference, 1e-3, 1.0);
                Penalty.Init(1.0, N);
                if (bHasWhite)
                {
                    Penalty[Emitters.PreferredWhiteIndex] = WhiteCost;
                }
            }

            FVector3d Apply(const TArray<double>& D) const
            {
                FVector3d Out = FVector3d::ZeroVector;
                for (int32 c = 0; c < N; ++c)
                {
                    Out.X += A[0 * N + c] * D[c];
                    Out.Y += A[1 * N + c] * D[c];
                    Out.Z += A[2 * N + c] * D[c];
                }
                return Out;
            }

            /** 非負のレベルで厳密に再現できるか（NNLS の相対残差で判定） */
            bool IsFeasible(const FVector3d& Target, TArray<double>* OutLevels = nullptr) const
            {
                TArray<double> B = { Target.X, Target.Y, Target.Z };
                TArray<double> D;
                const double Res = SolveNNLS(A, 3, N, B, D);
                if (OutLevels)
                {
                    *OutLevels = MoveTemp(D);
                }
                return Res <= FeasibleRelTol * FMath::Max(Target.Size(), 1e-12);
            }

            /**
             * 再現できる目標に対して、色を厳密に保ったまま Σ c_i d_i² が最小の解を求める。
             *  (1) [w·A ; sqrt(c)·I] d ≈ [w·t ; 0] を NNLS で解いて受動集合（d>0 の集合）を得る
             *  (2) その集合上で 等式制約付き最小ノルム解 d_P = C⁻¹A_Pᵀ(A_P C⁻¹ A_Pᵀ)⁻¹ t を厳密に求める
             */
            TArray<double> SolveExactPreferred(const FVector3d& Target, const TArray<double>& FeasibleLevels) const
            {
                const int32 Rows = 3 + N;
                const double Wt = ExactnessWeight / ColumnScale;
                TArray<double> Aug;
                Aug.SetNumZeroed(Rows * N);
                TArray<double> B;
                B.SetNumZeroed(Rows);
                for (int32 r = 0; r < 3; ++r)
                {
                    for (int32 c = 0; c < N; ++c)
                    {
                        Aug[r * N + c] = Wt * A[r * N + c];
                    }
                }
                B[0] = Wt * Target.X;
                B[1] = Wt * Target.Y;
                B[2] = Wt * Target.Z;
                for (int32 c = 0; c < N; ++c)
                {
                    Aug[(3 + c) * N + c] = FMath::Sqrt(Penalty[c]);
                }

                TArray<double> Weighted;
                SolveNNLS(Aug, Rows, N, B, Weighted);

                const double Tol = FeasibleRelTol * FMath::Max(Target.Size(), 1e-12);

                // (2) 受動集合上で厳密化
                TArray<int32> P;
                for (int32 c = 0; c < N; ++c)
                {
                    if (Weighted[c] > 0.0)
                    {
                        P.Add(c);
                    }
                }
                if (P.Num() > 0)
                {
                    TArray<double> Refined;
                    Refined.Init(0.0, N);
                    bool bRefinedOk = false;

                    // G = A_P C⁻¹ A_Pᵀ（3x3）
                    FMatrix3x3 G;
                    for (int32 r = 0; r < 3; ++r)
                    {
                        for (int32 s = 0; s < 3; ++s)
                        {
                            double Sum = 0.0;
                            for (int32 c : P)
                            {
                                Sum += A[r * N + c] * A[s * N + c] / Penalty[c];
                            }
                            G.M[r][s] = Sum;
                        }
                    }
                    FMatrix3x3 GInv;
                    if (P.Num() >= 3 && G.Inverse(GInv))
                    {
                        const FVector3d Lambda = GInv.Transform(Target);
                        for (int32 c : P)
                        {
                            Refined[c] = (A[0 * N + c] * Lambda.X + A[1 * N + c] * Lambda.Y + A[2 * N + c] * Lambda.Z) / Penalty[c];
                        }
                        bRefinedOk = true;
                    }
                    else
                    {
                        // 受動集合が 3 本未満（または退化）: その列だけの最小二乗で十分厳密
                        TArray<double> B3 = { Target.X, Target.Y, Target.Z };
                        double SubS[MaxNNLSCols];
                        SolveSubsetLeastSquares(A, 3, N, B3, P.GetData(), P.Num(), SubS);
                        for (int32 k = 0; k < P.Num(); ++k)
                        {
                            Refined[P[k]] = SubS[k];
                        }
                        bRefinedOk = true;
                    }

                    if (bRefinedOk)
                    {
                        double MinLevel = 0.0;
                        for (double V : Refined)
                        {
                            MinLevel = FMath::Min(MinLevel, V);
                        }
                        if (MinLevel >= -1e-9 * FMath::Max(Target.Size(), 1.0))
                        {
                            for (double& V : Refined)
                            {
                                V = FMath::Max(V, 0.0);
                            }
                            if ((Apply(Refined) - Target).Size() <= Tol)
                            {
                                return Refined;
                            }
                        }
                    }
                }

                // 厳密化できなければ、重み付き解（十分に近ければ）か、素の NNLS 解を使う
                if ((Apply(Weighted) - Target).Size() <= Tol)
                {
                    return Weighted;
                }
                return FeasibleLevels;
            }
        };
    }

    FSolveResult SolveEmitterLevels(const FEmitterSet& Emitters, const FVector3d& TargetCameraRGB)
    {
        FSolveResult Result;
        const FSolverContext Ctx(Emitters);
        Result.Levels.Init(0.0, Emitters.Responses.Num());

        const double TargetSize = TargetCameraRGB.Size();
        if (Ctx.N == 0 || TargetSize <= 1e-12)
        {
            Result.bOutOfGamut = (Ctx.N == 0 && TargetSize > 1e-12);
            Result.Achieved = FVector3d::ZeroVector;
            Result.Residual = TargetSize;
            return Result;
        }

        TArray<double> Levels;
        TArray<double> Feasible;
        if (Ctx.IsFeasible(TargetCameraRGB, &Feasible))
        {
            Levels = Ctx.SolveExactPreferred(TargetCameraRGB, Feasible);
        }
        else
        {
            // --- 色域外: 同じ輝度の白へ向かう直線上で、再現できる最も彩度の高い点を探す ---
            Result.bOutOfGamut = true;

            const FVector3d White = Emitters.GetWhiteResponse();
            const double LumWhite = FVector3d::DotProduct(White, Emitters.LuminanceWeights);
            const double LumTarget = FVector3d::DotProduct(TargetCameraRGB, Emitters.LuminanceWeights);

            bool bMapped = false;
            if (LumWhite > 1e-12 && LumTarget > 1e-12)
            {
                const FVector3d WhiteAtLum = White * (LumTarget / LumWhite);
                if (Ctx.IsFeasible(WhiteAtLum))
                {
                    // 実現可能な α の集合は [α*, 1] の区間（錐が凸なので）。二分探索で α* を求める
                    double Lo = 0.0;
                    double Hi = 1.0;
                    for (int32 Iter = 0; Iter < 48; ++Iter)
                    {
                        const double Mid = 0.5 * (Lo + Hi);
                        if (Ctx.IsFeasible(FMath::Lerp(TargetCameraRGB, WhiteAtLum, Mid)))
                        {
                            Hi = Mid;
                        }
                        else
                        {
                            Lo = Mid;
                        }
                    }
                    const FVector3d Mapped = FMath::Lerp(TargetCameraRGB, WhiteAtLum, Hi);
                    TArray<double> MappedFeasible;
                    Ctx.IsFeasible(Mapped, &MappedFeasible);
                    Levels = Ctx.SolveExactPreferred(Mapped, MappedFeasible);
                    bMapped = true;
                }
            }
            if (!bMapped)
            {
                // 輝度が定義できない（負の輝度など）・白が再現できない: 最小二乗で最も近い解
                Levels = MoveTemp(Feasible);
            }
        }

        // --- 1 を超えたら全体を等倍で縮小（比率を保つ） ---
        double MaxLevel = 0.0;
        for (double V : Levels)
        {
            MaxLevel = FMath::Max(MaxLevel, V);
        }
        if (MaxLevel > 1.0)
        {
            const double Scale = 1.0 / MaxLevel;
            for (double& V : Levels)
            {
                V *= Scale;
            }
            Result.bClipped = true;
        }

        for (int32 c = 0; c < Ctx.N; ++c)
        {
            Result.Levels[c] = FMath::Clamp(Levels[c], 0.0, 1.0);
        }
        TArray<double> Used = Result.Levels;
        Used.SetNum(Ctx.N);
        Result.Achieved = Ctx.Apply(Used);
        Result.Residual = (Result.Achieved - TargetCameraRGB).Size();
        return Result;
    }

    // =========================================================================
    // 4. 調光カーブ
    // =========================================================================

    namespace
    {
        using FLUTPoints = TArray<FVector2d, TInlineAllocator<32>>;

        /** X で並べ替え、Y を累積最大で単調非減少にした LUT を作る */
        void BuildMonotoneLUT(const TArray<FVector2d>& Points, FLUTPoints& Out)
        {
            Out.Reset();
            Out.Append(Points);
            Out.Sort([](const FVector2d& A, const FVector2d& B) { return A.X < B.X; });
            for (int32 i = 1; i < Out.Num(); ++i)
            {
                Out[i].Y = FMath::Max(Out[i].Y, Out[i - 1].Y);
            }
        }
    }

    double FDimmingCurve::Apply(double Level) const
    {
        const double L = FMath::Clamp(Level, 0.0, 1.0);
        switch (Type)
        {
        case EType::Gamma:
            return FMath::Pow(L, FMath::Max(Gamma, 1e-3));

        case EType::LUT:
        {
            if (Points.Num() < 2)
            {
                return L;
            }
            FLUTPoints Lut;
            BuildMonotoneLUT(Points, Lut);
            if (L <= Lut[0].X)
            {
                return Lut[0].Y;
            }
            for (int32 i = 0; i + 1 < Lut.Num(); ++i)
            {
                if (L <= Lut[i + 1].X)
                {
                    const double Span = Lut[i + 1].X - Lut[i].X;
                    const double T = (Span > 0.0) ? (L - Lut[i].X) / Span : 1.0;
                    return FMath::Lerp(Lut[i].Y, Lut[i + 1].Y, T);
                }
            }
            return Lut.Last().Y;
        }

        case EType::Linear:
        default:
            return L;
        }
    }

    double FDimmingCurve::Invert(double OutputFraction) const
    {
        const double O = FMath::Clamp(OutputFraction, 0.0, 1.0);
        switch (Type)
        {
        case EType::Gamma:
            return FMath::Pow(O, 1.0 / FMath::Max(Gamma, 1e-3));

        case EType::LUT:
        {
            if (Points.Num() < 2)
            {
                return O;
            }
            FLUTPoints Lut;
            BuildMonotoneLUT(Points, Lut);
            if (O <= Lut[0].Y)
            {
                return Lut[0].X;
            }
            // O > Y[i] が保証された状態で Y[i+1] ≥ O となる最初の区間を探す（平坦区間は飛ばされる）
            for (int32 i = 0; i + 1 < Lut.Num(); ++i)
            {
                if (O <= Lut[i + 1].Y)
                {
                    const double Span = Lut[i + 1].Y - Lut[i].Y;
                    const double T = (Span > 0.0) ? (O - Lut[i].Y) / Span : 0.0;
                    return FMath::Lerp(Lut[i].X, Lut[i + 1].X, T);
                }
            }
            return Lut.Last().X;
        }

        case EType::Linear:
        default:
            return O;
        }
    }

    // =========================================================================
    // 5. DMX 量子化
    // =========================================================================

    void QuantizeToDMX(double Level, bool b16Bit, uint8& Coarse, uint8& Fine)
    {
        const double L = FMath::Clamp(Level, 0.0, 1.0);
        if (b16Bit)
        {
            const uint32 V = (uint32)FMath::Clamp<int64>(FMath::RoundToInt64(L * 65535.0), 0, 65535);
            Coarse = (uint8)(V >> 8);
            Fine = (uint8)(V & 0xFF);
        }
        else
        {
            Coarse = (uint8)FMath::Clamp<int64>(FMath::RoundToInt64(L * 255.0), 0, 255);
            Fine = 0;
        }
    }

    double DequantizeDMX(uint8 Coarse, uint8 Fine, bool b16Bit)
    {
        if (b16Bit)
        {
            return (double)(((uint32)Coarse << 8) | (uint32)Fine) / 65535.0;
        }
        return (double)Coarse / 255.0;
    }

    // =========================================================================
    // 6. カメラ伝達関数の線形化
    // =========================================================================

    double LinearizeCameraValue(ECameraTransfer Transfer, double CodeValue01)
    {
        const double V = CodeValue01;
        switch (Transfer)
        {
        case ECameraTransfer::sRGB:
        {
            // IEC 61966-2-1:1999（負値は符号を保って対称に扱う）
            const double A = FMath::Abs(V);
            const double Lin = (A <= 0.04045) ? A / 12.92 : FMath::Pow((A + 0.055) / 1.055, 2.4);
            return (V < 0.0) ? -Lin : Lin;
        }

        case ECameraTransfer::Rec709:
        {
            // ITU-R BT.709-6 の OETF の逆
            const double A = FMath::Abs(V);
            const double Lin = (A < 0.081) ? A / 4.5 : FMath::Pow((A + 0.099) / 1.099, 1.0 / 0.45);
            return (V < 0.0) ? -Lin : Lin;
        }

        case ECameraTransfer::ArriLogC3_EI800:
        {
            // ARRI "ALEXA Log C Curve – Usage in VFX"（Harald Brendel, 2017）
            // EI 800、シーンリニア版のパラメータ。18% グレー → 0.391007
            constexpr double Cut = 0.010591;
            constexpr double A = 5.555556;
            constexpr double B = 0.052272;
            constexpr double C = 0.247190;
            constexpr double D = 0.385537;
            constexpr double E = 5.367655;
            constexpr double F = 0.092809;
            if (V > E * Cut + F)
            {
                return (FMath::Pow(10.0, (V - D) / C) - B) / A;
            }
            return (V - F) / E;
        }

        case ECameraTransfer::ArriLogC4:
        {
            // ARRI "LogC4 Logarithmic Color Space – Specification"（2022）
            // 18% グレー → 0.278396
            const double A = (FMath::Pow(2.0, 18.0) - 16.0) / 117.45;
            const double B = (1023.0 - 95.0) / 1023.0;
            const double C = 95.0 / 1023.0;
            const double S = (7.0 * FMath::Loge(2.0) * FMath::Pow(2.0, 7.0 - 14.0 * C / B)) / (A * B);
            const double T = (FMath::Pow(2.0, 14.0 * (-C / B) + 6.0) - 64.0) / A;
            if (V >= 0.0)
            {
                return (FMath::Pow(2.0, 14.0 * (V - C) / B + 6.0) - 64.0) / A;
            }
            return V * S + T;
        }

        case ECameraTransfer::SonySLog3:
        {
            // Sony "Technical Summary for S-Gamut3.Cine/S-Log3 and S-Gamut3/S-Log3"
            // 18% グレー → 420 / 1023
            const double CV = V * 1023.0;
            if (CV >= 171.2102946929)
            {
                return FMath::Pow(10.0, (CV - 420.0) / 261.5) * (0.18 + 0.01) - 0.01;
            }
            return (CV - 95.0) * 0.01125000 / (171.2102946929 - 95.0);
        }

        case ECameraTransfer::Linear:
        default:
            return V;
        }
    }

    FVector3d LinearizeCameraRGB(ECameraTransfer Transfer, const FVector3d& CodeValues01)
    {
        return FVector3d(
            LinearizeCameraValue(Transfer, CodeValues01.X),
            LinearizeCameraValue(Transfer, CodeValues01.Y),
            LinearizeCameraValue(Transfer, CodeValues01.Z));
    }

    // =========================================================================
    // 7. キャリブレーション用フィッティング
    // =========================================================================

    bool Fit3x3FromPatches(const TArray<FVector3d>& ReferenceRGB, const TArray<FVector3d>& MeasuredRGB,
                           FMatrix3x3& OutMatrix, double* OutRmsError)
    {
        OutMatrix = FMatrix3x3();
        if (OutRmsError)
        {
            *OutRmsError = 0.0;
        }
        const int32 Count = FMath::Min(ReferenceRGB.Num(), MeasuredRGB.Num());
        if (Count < 3)
        {
            return false;
        }

        // 正規方程式: M = (Σ m rᵀ)(Σ r rᵀ)⁻¹
        FMatrix3x3 Srr, Smr;
        for (int32 R = 0; R < 3; ++R)
        {
            for (int32 C = 0; C < 3; ++C)
            {
                Srr.M[R][C] = 0.0;
                Smr.M[R][C] = 0.0;
            }
        }
        for (int32 i = 0; i < Count; ++i)
        {
            const FVector3d& Ref = ReferenceRGB[i];
            const FVector3d& Mea = MeasuredRGB[i];
            for (int32 R = 0; R < 3; ++R)
            {
                for (int32 C = 0; C < 3; ++C)
                {
                    Srr.M[R][C] += Ref[R] * Ref[C];
                    Smr.M[R][C] += Mea[R] * Ref[C];
                }
            }
        }

        FMatrix3x3 SrrInv;
        if (!Srr.Inverse(SrrInv))
        {
            return false;
        }
        OutMatrix = Smr * SrrInv;

        if (OutRmsError)
        {
            double SumSq = 0.0;
            for (int32 i = 0; i < Count; ++i)
            {
                SumSq += (OutMatrix.Transform(ReferenceRGB[i]) - MeasuredRGB[i]).SizeSquared();
            }
            *OutRmsError = FMath::Sqrt(SumSq / Count);
        }
        return true;
    }

    double FitGammaFromSteps(const TArray<double>& Levels, const TArray<double>& Measured, double* OutScale)
    {
        if (OutScale)
        {
            *OutScale = 0.0;
        }
        const int32 Count = FMath::Min(Levels.Num(), Measured.Num());

        // ln(m) = ln(k) + γ·ln(L) の単回帰
        double SumX = 0.0, SumY = 0.0, SumXX = 0.0, SumXY = 0.0;
        int32 Valid = 0;
        for (int32 i = 0; i < Count; ++i)
        {
            if (Levels[i] <= 0.0 || Measured[i] <= 0.0)
            {
                continue;
            }
            const double X = FMath::Loge(Levels[i]);
            const double Y = FMath::Loge(Measured[i]);
            SumX += X;
            SumY += Y;
            SumXX += X * X;
            SumXY += X * Y;
            ++Valid;
        }
        if (Valid < 2)
        {
            return 0.0;
        }
        const double Denom = Valid * SumXX - SumX * SumX;
        if (FMath::Abs(Denom) <= 1e-15 * FMath::Max(Valid * SumXX, 1.0))
        {
            return 0.0;
        }
        const double GammaFit = (Valid * SumXY - SumX * SumY) / Denom;
        if (OutScale)
        {
            *OutScale = FMath::Exp((SumY - GammaFit * SumX) / Valid);
        }
        return GammaFit;
    }
}
