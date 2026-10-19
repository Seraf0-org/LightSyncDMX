// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncColorMath.h"
#include "ColorManagement/ColorSpace.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace LightSyncColor;

namespace LightSyncColorMathTests
{
    constexpr EAutomationTestFlags TestFlags = EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

    /** 値が期待値 ± 許容誤差に入っているか（失敗時は実際の値をログに出す） */
    bool Near(FAutomationTestBase& Test, const TCHAR* What, double Actual, double Expected, double Tolerance)
    {
        const bool bOk = FMath::Abs(Actual - Expected) <= Tolerance;
        if (!bOk)
        {
            Test.AddError(FString::Printf(TEXT("%s: 実際 %.9g / 期待 %.9g ± %.3g"), What, Actual, Expected, Tolerance));
        }
        return bOk;
    }

    bool NearVec(FAutomationTestBase& Test, const TCHAR* What, const FVector3d& Actual, const FVector3d& Expected, double Tolerance)
    {
        const bool bOk = (Actual - Expected).GetAbsMax() <= Tolerance;
        if (!bOk)
        {
            Test.AddError(FString::Printf(TEXT("%s: 実際 (%.9g, %.9g, %.9g) / 期待 (%.9g, %.9g, %.9g) ± %.3g"),
                What, Actual.X, Actual.Y, Actual.Z, Expected.X, Expected.Y, Expected.Z, Tolerance));
        }
        return bOk;
    }

    /** rg 色度（r = R/(R+G+B), g = G/(R+G+B)） */
    FVector2d Chromaticity(const FVector3d& RGB)
    {
        const double Sum = RGB.X + RGB.Y + RGB.Z;
        return FVector2d(RGB.X / Sum, RGB.Y / Sum);
    }

    /** 白の色度を中心とした色相角 [rad] */
    double HueAngle(const FVector3d& RGB, const FVector3d& White)
    {
        const FVector2d C = Chromaticity(RGB) - Chromaticity(White);
        return FMath::Atan2(C.Y, C.X);
    }

    double AngleDiff(double A, double B)
    {
        double D = A - B;
        while (D > UE_DOUBLE_PI) { D -= 2.0 * UE_DOUBLE_PI; }
        while (D < -UE_DOUBLE_PI) { D += 2.0 * UE_DOUBLE_PI; }
        return FMath::Abs(D);
    }

    FEmitterSet MakeRec709Identity()
    {
        FEmitterSet Set;
        Set.Responses = { FVector3d(1, 0, 0), FVector3d(0, 1, 0), FVector3d(0, 0, 1) };
        return Set;
    }

    FVector3d Achieve(const FEmitterSet& Set, const TArray<double>& Levels)
    {
        FVector3d Out = FVector3d::ZeroVector;
        for (int32 i = 0; i < Set.Responses.Num(); ++i)
        {
            Out += Set.Responses[i] * Levels[i];
        }
        return Out;
    }
}

using namespace LightSyncColorMathTests;

// =============================================================================
// 色空間
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathColorSpaceTest, "LightSync.ColorMath.ColorSpace", TestFlags)
bool FLightSyncColorMathColorSpaceTest::RunTest(const FString& Parameters)
{
    // 作業色空間の白 (1,1,1) は Y = 1、xy = 作業色空間の白色点
    const FVector3d WhiteXYZ = WorkingRGBToXYZ(FVector3d(1, 1, 1));
    const FVector3d WhitexyY = XYZToxyY(WhiteXYZ);
    const FVector2d WorkingWhite = UE::Color::FColorSpace::GetWorking().GetWhiteChromaticity();
    Near(*this, TEXT("白の Y"), WhitexyY.Z, 1.0, 1e-6);
    Near(*this, TEXT("白の x"), WhitexyY.X, WorkingWhite.X, 1e-6);
    Near(*this, TEXT("白の y"), WhitexyY.Y, WorkingWhite.Y, 1e-6);

    // RGB → XYZ → RGB の往復
    const FVector3d RGB(0.7, 0.2, 0.05);
    NearVec(*this, TEXT("RGB↔XYZ 往復"), XYZToWorkingRGB(WorkingRGBToXYZ(RGB)), RGB, 1e-12);

    // xyY 往復
    const FVector3d XYZ(0.4, 0.35, 0.2);
    NearVec(*this, TEXT("XYZ↔xyY 往復"), xyYToXYZ(XYZToxyY(XYZ)), XYZ, 1e-12);

    // uv(1960) ↔ xy 往復
    const FVector2d xy(0.3127, 0.3290);
    const FVector2d Back = UV1960Toxy(xyToUV1960(xy));
    Near(*this, TEXT("xy↔uv 往復 x"), Back.X, xy.X, 1e-12);
    Near(*this, TEXT("xy↔uv 往復 y"), Back.Y, xy.Y, 1e-12);
    return true;
}

// =============================================================================
// CCT / Duv
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathCCTD65Test, "LightSync.ColorMath.CCT.D65", TestFlags)
bool FLightSyncColorMathCCTD65Test::RunTest(const FString& Parameters)
{
    // D65 (0.3127, 0.3290) → CCT 6504K, Duv 0.0032（Ohno 2013 の参照実装で 6504.31K / 0.003207）
    double CCT = 0.0, Duv = 0.0;
    TestTrue(TEXT("XYZToCCTDuv 成功"), XYZToCCTDuv(xyYToXYZ(FVector3d(0.3127, 0.3290, 1.0)), CCT, Duv));
    Near(*this, TEXT("D65 CCT"), CCT, 6504.31, 1.0);
    Near(*this, TEXT("D65 Duv"), Duv, 0.003207, 0.0001);

    // 黒は失敗を返す
    TestFalse(TEXT("黒は CCT なし"), XYZToCCTDuv(FVector3d::ZeroVector, CCT, Duv));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathCCTRoundTripTest, "LightSync.ColorMath.CCT.PlanckRoundTrip", TestFlags)
bool FLightSyncColorMathCCTRoundTripTest::RunTest(const FString& Parameters)
{
    // Planck 軌跡の絶対位置（1nm の CIE 1931 等色関数で計算した参照値）
    struct FRef { double CCT; double Duv; double X; double Y; };
    const FRef Refs[] =
    {
        { 2700.0,  0.000, 0.45986032, 0.41060103 },
        { 5600.0,  0.000, 0.33019914, 0.33908124 },
        { 5600.0,  0.010, 0.33004330, 0.35906961 },
        { 3200.0, -0.008, 0.41344483, 0.37634598 },
    };
    for (const FRef& R : Refs)
    {
        const FVector2d xy = CCTDuvToxy(R.CCT, R.Duv);
        Near(*this, *FString::Printf(TEXT("%.0fK Duv %.3f の x"), R.CCT, R.Duv), xy.X, R.X, 2e-5);
        Near(*this, *FString::Printf(TEXT("%.0fK Duv %.3f の y"), R.CCT, R.Duv), xy.Y, R.Y, 2e-5);

        double CCT = 0.0, Duv = 0.0;
        XYZToCCTDuv(xyYToXYZ(FVector3d(xy.X, xy.Y, 1.0)), CCT, Duv);
        Near(*this, *FString::Printf(TEXT("%.0fK Duv %.3f の往復 CCT"), R.CCT, R.Duv), CCT, R.CCT, 1.0);
        Near(*this, *FString::Printf(TEXT("%.0fK Duv %.3f の往復 Duv"), R.CCT, R.Duv), Duv, R.Duv, 1e-5);
    }

    // 軌跡上の往復をレンジ全体で（2700–10000K は 1K 未満、それ以外も 0.1% 未満）
    const double Temps[] = { 1200.0, 2000.0, 2700.0, 3200.0, 4300.0, 5600.0, 6500.0, 8000.0, 10000.0, 15000.0, 24000.0 };
    for (double T : Temps)
    {
        const FVector2d xy = CCTDuvToxy(T, 0.0);
        double CCT = 0.0, Duv = 0.0;
        XYZToCCTDuv(xyYToXYZ(FVector3d(xy.X, xy.Y, 1.0)), CCT, Duv);
        const double Tol = (T >= 2700.0 && T <= 10000.0) ? 1.0 : T * 1e-3;
        Near(*this, *FString::Printf(TEXT("%.0fK 往復 CCT"), T), CCT, T, Tol);
        Near(*this, *FString::Printf(TEXT("%.0fK 往復 Duv"), T), Duv, 0.0, 1e-5);
    }
    return true;
}

// =============================================================================
// ソルバ
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathSolverIdentityTest, "LightSync.ColorMath.Solver.IdentityExact", TestFlags)
bool FLightSyncColorMathSolverIdentityTest::RunTest(const FString& Parameters)
{
    // Rec.709 理想応答の RGB 灯体: 色域内の目標はそのままのレベルで厳密に再現される
    const FEmitterSet Set = MakeRec709Identity();
    const FVector3d Targets[] =
    {
        FVector3d(0.2, 0.5, 0.7), FVector3d(1.0, 0.0, 0.0), FVector3d(0.0, 0.3, 0.0),
        FVector3d(0.9, 0.9, 0.9), FVector3d(0.01, 0.002, 0.03), FVector3d(1.0, 1.0, 1.0),
    };
    for (const FVector3d& T : Targets)
    {
        const FSolveResult R = SolveEmitterLevels(Set, T);
        TestEqual(TEXT("レベル数"), R.Levels.Num(), 3);
        NearVec(*this, TEXT("レベル = 目標"), FVector3d(R.Levels[0], R.Levels[1], R.Levels[2]), T, 1e-12);
        NearVec(*this, TEXT("Achieved = 目標"), R.Achieved, T, 1e-12);
        TestFalse(TEXT("色域内"), R.bOutOfGamut);
        TestFalse(TEXT("クリップなし"), R.bClipped);
        Near(*this, TEXT("残差"), R.Residual, 0.0, 1e-12);
    }

    // 黒
    const FSolveResult Black = SolveEmitterLevels(Set, FVector3d::ZeroVector);
    NearVec(*this, TEXT("黒"), FVector3d(Black.Levels[0], Black.Levels[1], Black.Levels[2]), FVector3d::ZeroVector, 0.0);

    // 非理想の 3 原色（行列が正則）でも色域内なら厳密
    FEmitterSet Real;
    Real.Responses = { FVector3d(0.8, 0.15, 0.05), FVector3d(0.1, 0.8, 0.1), FVector3d(0.05, 0.1, 0.85) };
    const FVector3d Levels(0.3, 0.6, 0.2);
    const FVector3d Target = Achieve(Real, { Levels.X, Levels.Y, Levels.Z });
    const FSolveResult RR = SolveEmitterLevels(Real, Target);
    NearVec(*this, TEXT("非理想 RGB のレベル"), FVector3d(RR.Levels[0], RR.Levels[1], RR.Levels[2]), Levels, 1e-12);
    TestFalse(TEXT("非理想 RGB は色域内"), RR.bOutOfGamut);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathSolverRGBWTest, "LightSync.ColorMath.Solver.RGBWWhitePreference", TestFlags)
bool FLightSyncColorMathSolverRGBWTest::RunTest(const FString& Parameters)
{
    FEmitterSet Set = MakeRec709Identity();
    Set.Responses.Add(FVector3d(1.0, 1.0, 1.0));
    Set.PreferredWhiteIndex = 3;

    // 解析解: W = ΣT / (3 + c_w)（c_w = 1 − 優先度）、ただし W ≤ min(T) = 0.8
    const FVector3d Target(0.9, 0.85, 0.8);

    double PrevWhite = -1.0;
    for (double Pref : { 0.0, 0.5, 0.8, 1.0 })
    {
        Set.WhitePreference = Pref;
        const FSolveResult R = SolveEmitterLevels(Set, Target);
        TestEqual(TEXT("レベル数"), R.Levels.Num(), 4);
        NearVec(*this, *FString::Printf(TEXT("優先 %.1f: 厳密に一致"), Pref), R.Achieved, Target, 1e-9);
        TestFalse(TEXT("色域内"), R.bOutOfGamut);
        TestFalse(TEXT("クリップなし"), R.bClipped);
        TestTrue(*FString::Printf(TEXT("優先 %.1f: W > 0 (W=%.6f)"), Pref, R.Levels[3]), R.Levels[3] > 0.0);
        TestTrue(*FString::Printf(TEXT("優先 %.1f: 優先度を上げると W が増える (%.6f > %.6f)"), Pref, R.Levels[3], PrevWhite), R.Levels[3] > PrevWhite);
        for (double L : R.Levels)
        {
            TestTrue(TEXT("レベルは非負"), L >= 0.0);
        }
        PrevWhite = R.Levels[3];
    }

    // 優先度 0 は W = 2.55 / 4 = 0.6375、優先度 1 は上限（= 最小成分 0.8）まで使われる
    Set.WhitePreference = 0.0;
    Near(*this, TEXT("優先度 0 の W"), SolveEmitterLevels(Set, Target).Levels[3], 2.55 / 4.0, 1e-9);
    Set.WhitePreference = 1.0;
    Near(*this, TEXT("優先度 1 の W"), SolveEmitterLevels(Set, Target).Levels[3], 0.8, 1e-6);

    // 白の目標は（優先度 0.8 で）ほぼ W だけで出す
    Set.WhitePreference = 0.8;
    const FSolveResult White = SolveEmitterLevels(Set, FVector3d(0.5, 0.5, 0.5));
    NearVec(*this, TEXT("白目標の一致"), White.Achieved, FVector3d(0.5, 0.5, 0.5), 1e-9);
    TestTrue(*FString::Printf(TEXT("白目標で W が支配的 (W=%.4f)"), White.Levels[3]), White.Levels[3] > White.Levels[0] * 10.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathSolverGamutTest, "LightSync.ColorMath.Solver.OutOfGamutHue", TestFlags)
bool FLightSyncColorMathSolverGamutTest::RunTest(const FString& Parameters)
{
    // 色純度の低い 3 原色（実灯体の LED をカメラで見たイメージ）
    FEmitterSet Set;
    Set.Responses = { FVector3d(0.8, 0.15, 0.05), FVector3d(0.1, 0.8, 0.1), FVector3d(0.05, 0.1, 0.85) };
    const FVector3d White = Set.GetWhiteResponse();

    const FVector3d Targets[] =
    {
        FVector3d(0.6, 0.05, 0.02),   // 灯体より鮮やかな赤
        FVector3d(0.02, 0.5, 0.45),   // 鮮やかなシアン
        FVector3d(0.3, 0.0, 0.5),     // 鮮やかな紫
    };
    for (const FVector3d& T : Targets)
    {
        const FSolveResult R = SolveEmitterLevels(Set, T);
        TestTrue(TEXT("色域外フラグ"), R.bOutOfGamut);
        TestFalse(TEXT("クリップなし"), R.bClipped);

        // 白色点まわりの色相が保たれる
        const double Diff = AngleDiff(HueAngle(R.Achieved, White), HueAngle(T, White));
        TestTrue(*FString::Printf(TEXT("色相差 %.3g rad < 1e-4"), Diff), Diff < 1e-4);

        // 輝度が保たれる
        Near(*this, TEXT("輝度"), FVector3d::DotProduct(R.Achieved, Set.LuminanceWeights),
             FVector3d::DotProduct(T, Set.LuminanceWeights), 1e-6);

        // 彩度は下がるが、白にはなりきらない（境界まで）
        const double SatTarget = (Chromaticity(T) - Chromaticity(White)).Size();
        const double SatAchieved = (Chromaticity(R.Achieved) - Chromaticity(White)).Size();
        TestTrue(TEXT("彩度は目標以下"), SatAchieved < SatTarget);
        TestTrue(TEXT("彩度は残る"), SatAchieved > 0.05);

        // 境界上: どれかのエミッタはほぼ 0
        const double MinLevel = FMath::Min3(R.Levels[0], R.Levels[1], R.Levels[2]);
        Near(*this, TEXT("境界上（最小レベル ≈ 0）"), MinLevel, 0.0, 1e-5);
    }

    // 理想 RGB でも負の成分を持つ目標は色域外
    const FEmitterSet Ideal = MakeRec709Identity();
    const FVector3d Neg(0.8, 0.3, -0.1);
    const FSolveResult RN = SolveEmitterLevels(Ideal, Neg);
    TestTrue(TEXT("負成分は色域外"), RN.bOutOfGamut);
    const FVector3d IdealWhite(1, 1, 1);
    const double DiffN = AngleDiff(HueAngle(RN.Achieved, IdealWhite), HueAngle(Neg, IdealWhite));
    TestTrue(*FString::Printf(TEXT("負成分の色相差 %.3g rad"), DiffN), DiffN < 1e-4);
    Near(*this, TEXT("負成分の B は 0"), RN.Levels[2], 0.0, 1e-6);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathSolverClipTest, "LightSync.ColorMath.Solver.ClipPreservesRatios", TestFlags)
bool FLightSyncColorMathSolverClipTest::RunTest(const FString& Parameters)
{
    const FEmitterSet Set = MakeRec709Identity();
    const FSolveResult R = SolveEmitterLevels(Set, FVector3d(2.0, 1.0, 0.5));
    TestTrue(TEXT("クリップフラグ"), R.bClipped);
    TestFalse(TEXT("色域内"), R.bOutOfGamut);
    NearVec(*this, TEXT("等倍縮小"), FVector3d(R.Levels[0], R.Levels[1], R.Levels[2]), FVector3d(1.0, 0.5, 0.25), 1e-12);

    // RGBW でも比率（= 色度）は保たれ、最大レベルは 1
    FEmitterSet RGBW = MakeRec709Identity();
    RGBW.Responses.Add(FVector3d(1.0, 1.0, 1.0));
    RGBW.PreferredWhiteIndex = 3;
    RGBW.WhitePreference = 0.5;
    const FVector3d Target(3.0, 2.0, 1.5);
    const FSolveResult RW = SolveEmitterLevels(RGBW, Target);
    TestTrue(TEXT("RGBW クリップフラグ"), RW.bClipped);
    double MaxLevel = 0.0;
    for (double L : RW.Levels)
    {
        MaxLevel = FMath::Max(MaxLevel, L);
    }
    Near(*this, TEXT("RGBW 最大レベル"), MaxLevel, 1.0, 1e-12);
    const double Scale = RW.Achieved.X / Target.X;
    NearVec(*this, TEXT("RGBW 比率保持"), RW.Achieved, Target * Scale, 1e-9);
    return true;
}

// =============================================================================
// 調光カーブ / 量子化
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathDimmingTest, "LightSync.ColorMath.DimmingCurve", TestFlags)
bool FLightSyncColorMathDimmingTest::RunTest(const FString& Parameters)
{
    FDimmingCurve GammaCurve;
    GammaCurve.Type = FDimmingCurve::EType::Gamma;
    GammaCurve.Gamma = 2.2;
    Near(*this, TEXT("ガンマ Apply(0.5)"), GammaCurve.Apply(0.5), FMath::Pow(0.5, 2.2), 1e-12);
    for (double L = 0.0; L <= 1.0; L += 0.05)
    {
        Near(*this, TEXT("ガンマ往復"), GammaCurve.Invert(GammaCurve.Apply(L)), L, 1e-12);
    }

    FDimmingCurve Linear;
    Near(*this, TEXT("リニア"), Linear.Invert(0.37), 0.37, 0.0);

    // 区分線形 LUT（並び順は不定、平坦区間を含む）
    FDimmingCurve Lut;
    Lut.Type = FDimmingCurve::EType::LUT;
    Lut.Points = { FVector2d(1.0, 1.0), FVector2d(0.0, 0.0), FVector2d(0.5, 0.2), FVector2d(0.6, 0.2), FVector2d(0.25, 0.05) };
    Near(*this, TEXT("LUT Apply(0.375)"), Lut.Apply(0.375), 0.125, 1e-12);
    Near(*this, TEXT("LUT Apply(0.8)"), Lut.Apply(0.8), 0.6, 1e-12);
    Near(*this, TEXT("LUT Invert(0.125)"), Lut.Invert(0.125), 0.375, 1e-12);
    Near(*this, TEXT("LUT Invert(0.6)"), Lut.Invert(0.6), 0.8, 1e-12);
    Near(*this, TEXT("LUT 平坦区間は最小レベル"), Lut.Invert(0.2), 0.5, 1e-12);
    for (double L = 0.0; L <= 1.0; L += 0.05)
    {
        if (L > 0.5 && L <= 0.6)
        {
            continue; // 平坦区間は一意に戻らない
        }
        Near(*this, TEXT("LUT 往復"), Lut.Invert(Lut.Apply(L)), L, 1e-12);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathDMXTest, "LightSync.ColorMath.DMX.Quantize", TestFlags)
bool FLightSyncColorMathDMXTest::RunTest(const FString& Parameters)
{
    uint8 Coarse = 0, Fine = 0;

    QuantizeToDMX(1.0, true, Coarse, Fine);
    TestTrue(TEXT("16bit 1.0 → FF FF"), Coarse == 255 && Fine == 255);
    QuantizeToDMX(0.5, true, Coarse, Fine);
    TestTrue(TEXT("16bit 0.5 → 32768 (80 00)"), Coarse == 128 && Fine == 0);
    QuantizeToDMX(-0.2, true, Coarse, Fine);
    TestTrue(TEXT("16bit 負値 → 0"), Coarse == 0 && Fine == 0);
    QuantizeToDMX(0.5, false, Coarse, Fine);
    TestTrue(TEXT("8bit 0.5 → 128"), Coarse == 128 && Fine == 0);

    double MaxErr16 = 0.0;
    double MaxErr8 = 0.0;
    for (int32 i = 0; i <= 100000; ++i)
    {
        const double L = i / 100000.0;
        QuantizeToDMX(L, true, Coarse, Fine);
        MaxErr16 = FMath::Max(MaxErr16, FMath::Abs(DequantizeDMX(Coarse, Fine, true) - L));
        QuantizeToDMX(L, false, Coarse, Fine);
        MaxErr8 = FMath::Max(MaxErr8, FMath::Abs(DequantizeDMX(Coarse, Fine, false) - L));
    }
    TestTrue(*FString::Printf(TEXT("16bit 往復誤差 %.3g ≤ 1/65535"), MaxErr16), MaxErr16 <= 1.0 / 65535.0);
    TestTrue(*FString::Printf(TEXT("16bit は最近接に丸める (%.3g ≤ 0.5/65535)"), MaxErr16), MaxErr16 <= 0.5 / 65535.0 + 1e-12);
    TestTrue(*FString::Printf(TEXT("8bit は最近接に丸める (%.3g ≤ 0.5/255)"), MaxErr8), MaxErr8 <= 0.5 / 255.0 + 1e-12);
    return true;
}

// =============================================================================
// カメラ伝達関数
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathCameraTransferTest, "LightSync.ColorMath.CameraTransfer", TestFlags)
bool FLightSyncColorMathCameraTransferTest::RunTest(const FString& Parameters)
{
    // ARRI LogC3 (EI 800): 18% グレー = 0.391007（10bit で 400）、1.0 = 0.570632
    Near(*this, TEXT("LogC3 18%"), LinearizeCameraValue(ECameraTransfer::ArriLogC3_EI800, 0.391007), 0.18, 1e-5);
    Near(*this, TEXT("LogC3 100%"), LinearizeCameraValue(ECameraTransfer::ArriLogC3_EI800, 0.570632), 1.0, 1e-5);
    Near(*this, TEXT("LogC3 0.1%（直線部）"), LinearizeCameraValue(ECameraTransfer::ArriLogC3_EI800, 0.098177), 0.001, 1e-6);

    // ARRI LogC4: 18% グレー = 0.278396、1.0 = 0.427519、0 = 95/1023
    Near(*this, TEXT("LogC4 18%"), LinearizeCameraValue(ECameraTransfer::ArriLogC4, 0.278396), 0.18, 1e-5);
    Near(*this, TEXT("LogC4 100%"), LinearizeCameraValue(ECameraTransfer::ArriLogC4, 0.427519), 1.0, 1e-5);
    Near(*this, TEXT("LogC4 0"), LinearizeCameraValue(ECameraTransfer::ArriLogC4, 95.0 / 1023.0), 0.0, 1e-9);

    // Sony S-Log3: 18% グレー = 420/1023、90% 白 = 0.584453、0 = 95/1023
    Near(*this, TEXT("S-Log3 18%"), LinearizeCameraValue(ECameraTransfer::SonySLog3, 420.0 / 1023.0), 0.18, 1e-9);
    Near(*this, TEXT("S-Log3 90%"), LinearizeCameraValue(ECameraTransfer::SonySLog3, 0.584453), 0.9, 1e-5);
    Near(*this, TEXT("S-Log3 0"), LinearizeCameraValue(ECameraTransfer::SonySLog3, 95.0 / 1023.0), 0.0, 1e-9);

    // sRGB / Rec.709
    Near(*this, TEXT("sRGB 0.5"), LinearizeCameraValue(ECameraTransfer::sRGB, 0.5), 0.2140411405, 1e-9);
    Near(*this, TEXT("sRGB 0.02（直線部）"), LinearizeCameraValue(ECameraTransfer::sRGB, 0.02), 0.02 / 12.92, 1e-12);
    Near(*this, TEXT("Rec.709 0.5"), LinearizeCameraValue(ECameraTransfer::Rec709, 0.5), 0.2595894005, 1e-9);
    Near(*this, TEXT("Rec.709 0.05（直線部）"), LinearizeCameraValue(ECameraTransfer::Rec709, 0.05), 0.05 / 4.5, 1e-12);
    Near(*this, TEXT("Linear"), LinearizeCameraValue(ECameraTransfer::Linear, 0.42), 0.42, 0.0);

    // 区分の境目で連続
    const double Eps = 1e-9;
    const double LogC3Cut = 5.367655 * 0.010591 + 0.092809;
    Near(*this, TEXT("LogC3 境目の連続性"),
         LinearizeCameraValue(ECameraTransfer::ArriLogC3_EI800, LogC3Cut + Eps),
         LinearizeCameraValue(ECameraTransfer::ArriLogC3_EI800, LogC3Cut - Eps), 1e-6);
    const double SLog3Cut = 171.2102946929 / 1023.0;
    Near(*this, TEXT("S-Log3 境目の連続性"),
         LinearizeCameraValue(ECameraTransfer::SonySLog3, SLog3Cut + Eps),
         LinearizeCameraValue(ECameraTransfer::SonySLog3, SLog3Cut - Eps), 1e-6);
    Near(*this, TEXT("LogC4 境目の連続性"),
         LinearizeCameraValue(ECameraTransfer::ArriLogC4, Eps),
         LinearizeCameraValue(ECameraTransfer::ArriLogC4, -Eps), 1e-6);
    return true;
}

// =============================================================================
// フィッティング
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathFitGammaTest, "LightSync.ColorMath.Fit.Gamma", TestFlags)
bool FLightSyncColorMathFitGammaTest::RunTest(const FString& Parameters)
{
    const TArray<double> Levels = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    TArray<double> Measured;
    for (double L : Levels)
    {
        Measured.Add(0.8 * FMath::Pow(L, 2.2));
    }
    double Scale = 0.0;
    Near(*this, TEXT("ガンマ"), FitGammaFromSteps(Levels, Measured, &Scale), 2.2, 1e-9);
    Near(*this, TEXT("スケール"), Scale, 0.8, 1e-9);

    // 有効点が足りない
    Near(*this, TEXT("点不足は 0"), FitGammaFromSteps({ 0.5 }, { 0.2 }), 0.0, 0.0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncColorMathFit3x3Test, "LightSync.ColorMath.Fit.Matrix3x3", TestFlags)
bool FLightSyncColorMathFit3x3Test::RunTest(const FString& Parameters)
{
    FMatrix3x3 Known;
    const double Values[3][3] = { { 0.92, 0.11, 0.03 }, { 0.07, 0.85, 0.12 }, { 0.01, 0.06, 0.97 } };
    for (int32 R = 0; R < 3; ++R)
    {
        for (int32 C = 0; C < 3; ++C)
        {
            Known.M[R][C] = Values[R][C];
        }
    }

    const TArray<FVector3d> Reference =
    {
        FVector3d(1, 0, 0), FVector3d(0, 1, 0), FVector3d(0, 0, 1), FVector3d(1, 1, 1),
        FVector3d(0.5, 0.2, 0.1), FVector3d(0.1, 0.6, 0.3), FVector3d(0.18, 0.18, 0.18), FVector3d(0.3, 0.05, 0.8),
    };
    TArray<FVector3d> Measured;
    for (const FVector3d& Ref : Reference)
    {
        Measured.Add(Known.Transform(Ref));
    }

    FMatrix3x3 Fit;
    double Rms = -1.0;
    TestTrue(TEXT("フィット成功"), Fit3x3FromPatches(Reference, Measured, Fit, &Rms));
    for (int32 R = 0; R < 3; ++R)
    {
        for (int32 C = 0; C < 3; ++C)
        {
            Near(*this, *FString::Printf(TEXT("M[%d][%d]"), R, C), Fit.M[R][C], Values[R][C], 1e-9);
        }
    }
    Near(*this, TEXT("RMS"), Rms, 0.0, 1e-9);

    // 逆行列
    FMatrix3x3 Inv;
    TestTrue(TEXT("逆行列"), Fit.Inverse(Inv));
    const FMatrix3x3 Id = Fit * Inv;
    for (int32 R = 0; R < 3; ++R)
    {
        for (int32 C = 0; C < 3; ++C)
        {
            Near(*this, TEXT("M·M⁻¹ = I"), Id.M[R][C], R == C ? 1.0 : 0.0, 1e-12);
        }
    }

    // 縮退した入力は失敗
    TestFalse(TEXT("2 パッチは失敗"), Fit3x3FromPatches({ FVector3d(1, 0, 0), FVector3d(0, 1, 0) },
                                                        { FVector3d(1, 0, 0), FVector3d(0, 1, 0) }, Fit));
    TestFalse(TEXT("同一平面は失敗"), Fit3x3FromPatches({ FVector3d(1, 0, 0), FVector3d(0, 1, 0), FVector3d(1, 1, 0) },
                                                        { FVector3d(1, 0, 0), FVector3d(0, 1, 0), FVector3d(1, 1, 0) }, Fit));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
