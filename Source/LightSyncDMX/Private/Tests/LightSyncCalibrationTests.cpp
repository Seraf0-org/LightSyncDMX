// Copyright UE-Comp. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "LightSyncCalibrator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace LightSyncCalibrationTests
{
    using LightSyncColor::FMatrix3x3;

    // 実在しそうな「真の」灯体: エミッタに混色の漏れがあり、環境光の黒レベルもある
    const FVector3d TrueR(0.90, 0.08, 0.01);
    const FVector3d TrueG(0.10, 0.85, 0.05);
    const FVector3d TrueB(0.02, 0.07, 0.95);
    const FVector3d TrueW(0.95, 1.00, 0.90);
    const FVector3d Ambient(0.010, 0.011, 0.012);

    FLinearColor ToColor(const FVector3d &V)
    {
        return FLinearColor(static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z), 1.0f);
    }

    FLinearColor EncodeSRGB(const FVector3d &V)
    {
        auto E = [](double X) { return X <= 0.0031308 ? 12.92 * X : 1.055 * FMath::Pow(X, 1.0 / 2.4) - 0.055; };
        return FLinearColor(static_cast<float>(E(V.X)), static_cast<float>(E(V.Y)), static_cast<float>(E(V.Z)), 1.0f);
    }

    bool Near(const FLinearColor &A, const FVector3d &B, double Tol)
    {
        return FMath::Abs(A.R - B.X) < Tol && FMath::Abs(A.G - B.Y) < Tol && FMath::Abs(A.B - B.Z) < Tol;
    }

    /** 真の灯体をカメラで撮った測定値を作る */
    TArray<FLightSyncCalibrationStep> MeasureFixture(const FLightSyncFixtureProfileData &Profile, TFunctionRef<double(double)> Curve,
                                                     TFunctionRef<FLinearColor(const FVector3d &)> Camera)
    {
        const TArray<FVector3d> Responses = {TrueR, TrueG, TrueB, TrueW};
        FVector3d Sum = FVector3d::ZeroVector;
        for (int32 Index = 0; Index < Profile.Emitters.Num(); ++Index)
        {
            Sum += Responses[Index];
        }

        TArray<FLightSyncCalibrationStep> Steps = ULightSyncCalibrator::MakeFixtureSteps(Profile);
        for (FLightSyncCalibrationStep &Step : Steps)
        {
            FVector3d Linear = Ambient;
            if (Step.Kind == ELightSyncCalibrationStepKind::FixtureEmitter)
            {
                Linear += Responses[Step.EmitterIndex];
            }
            else if (Step.Kind == ELightSyncCalibrationStepKind::FixtureAllLevel)
            {
                Linear += Sum * Curve(Step.Level);
            }
            Step.CameraCode = Camera(Linear);
            Step.bMeasured = true;
        }
        return Steps;
    }
}

using namespace LightSyncCalibrationTests;

#define LS_CAL_TEST(Class, Name) \
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "LightSync.Calibration." Name, \
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

LS_CAL_TEST(FLightSyncCalStepsTest, "StepsFollowProfile")
bool FLightSyncCalStepsTest::RunTest(const FString &Parameters)
{
    const TArray<FLightSyncCalibrationStep> RGBW = ULightSyncCalibrator::MakeFixtureSteps(LightSyncFixture::MakePreset(ELightSyncColorMode::RGBW));
    TestEqual(TEXT("RGBW: off + 4 emitters + 4 levels"), RGBW.Num(), 9);
    const TArray<FLightSyncCalibrationStep> CCT = ULightSyncCalibrator::MakeFixtureSteps(LightSyncFixture::MakePreset(ELightSyncColorMode::CCT));
    TestEqual(TEXT("CCT: off + white + 4 levels"), CCT.Num(), 6);
    TestEqual(TEXT("wall: black + R G B W + 4 gray"), ULightSyncCalibrator::MakeWallSteps().Num(), 9);
    return true;
}

LS_CAL_TEST(FLightSyncCalFixtureGammaTest, "FixtureFitRecoversResponsesAndGamma")
bool FLightSyncCalFixtureGammaTest::RunTest(const FString &Parameters)
{
    const FLightSyncFixtureProfileData Base = LightSyncFixture::MakePreset(ELightSyncColorMode::RGBW);
    const TArray<FLightSyncCalibrationStep> Steps = MeasureFixture(Base, [](double L) { return FMath::Pow(L, 2.0); },
                                                                   [](const FVector3d &V) { return ToColor(V); });

    FLightSyncFixtureProfileData Fitted;
    FString Report;
    TestTrue(TEXT("fit ok"), ULightSyncCalibrator::FitFixtureFromSteps(Base, Steps, ELightSyncCameraTransfer::Linear, Fitted, Report));
    AddInfo(Report);

    const TArray<FVector3d> Truth = {TrueR, TrueG, TrueB, TrueW};
    for (int32 Index = 0; Index < 4; ++Index)
    {
        TestTrue(FString::Printf(TEXT("emitter %d response"), Index), Near(Fitted.Emitters[Index].CameraResponse, Truth[Index], 1e-5));
    }
    TestTrue(TEXT("gamma curve chosen"), Fitted.DimmingCurve == ELightSyncDimmingCurve::Gamma);
    TestTrue(FString::Printf(TEXT("gamma %.4f"), Fitted.DimmingGamma), FMath::IsNearlyEqual(Fitted.DimmingGamma, 2.0f, 0.01f));
    return true;
}

LS_CAL_TEST(FLightSyncCalFixtureSRGBCameraTest, "FixtureFitLinearizesCameraCodes")
bool FLightSyncCalFixtureSRGBCameraTest::RunTest(const FString &Parameters)
{
    // カメラが sRGB で記録していても、線形化してから引き算・フィットする
    const FLightSyncFixtureProfileData Base = LightSyncFixture::MakePreset(ELightSyncColorMode::RGB);
    const TArray<FLightSyncCalibrationStep> Steps = MeasureFixture(Base, [](double L) { return L; },
                                                                   [](const FVector3d &V) { return EncodeSRGB(V * 0.5); });

    FLightSyncFixtureProfileData Fitted;
    FString Report;
    TestTrue(TEXT("fit ok"), ULightSyncCalibrator::FitFixtureFromSteps(Base, Steps, ELightSyncCameraTransfer::SRGB, Fitted, Report));
    TestTrue(TEXT("R response (x0.5 exposure)"), Near(Fitted.Emitters[0].CameraResponse, TrueR * 0.5, 2e-4));
    TestTrue(TEXT("linear curve"), Fitted.DimmingCurve == ELightSyncDimmingCurve::Linear);
    return true;
}

LS_CAL_TEST(FLightSyncCalFixtureLUTTest, "NonPowerCurveBecomesLUT")
bool FLightSyncCalFixtureLUTTest::RunTest(const FString &Parameters)
{
    // S 字カーブはガンマで近似できない → LUT
    auto SCurve = [](double L) { return L * L * (3.0 - 2.0 * L); };
    const FLightSyncFixtureProfileData Base = LightSyncFixture::MakePreset(ELightSyncColorMode::DRGB);
    const TArray<FLightSyncCalibrationStep> Steps = MeasureFixture(Base, SCurve, [](const FVector3d &V) { return ToColor(V); });

    FLightSyncFixtureProfileData Fitted;
    FString Report;
    TestTrue(TEXT("fit ok"), ULightSyncCalibrator::FitFixtureFromSteps(Base, Steps, ELightSyncCameraTransfer::Linear, Fitted, Report));
    TestTrue(TEXT("LUT chosen"), Fitted.DimmingCurve == ELightSyncDimmingCurve::LUT);
    TestEqual(TEXT("LUT points (0 + 4 levels)"), Fitted.DimmingLUT.Num(), 5);
    if (Fitted.DimmingLUT.Num() == 5)
    {
        TestTrue(TEXT("LUT 50%"), FMath::IsNearlyEqual(Fitted.DimmingLUT[2].Y, SCurve(0.5), 1e-4));
    }

    // 校正後のプロファイルで 25% の光を出すと、LUT を逆に引いてレベル 0.25 付近になる
    const FLightSyncFixtureOutput Out = LightSyncFixture::Compute(Fitted, FLinearColor(0.0f, 0.0f, 0.0f), FMatrix3x3::Identity(), 1.0);
    TestEqual(TEXT("black target gives dimmer 0"), Out.Values[0], static_cast<uint8>(0));
    return true;
}

LS_CAL_TEST(FLightSyncCalStageTest, "StageFitRecoversReferenceMatrix")
bool FLightSyncCalStageTest::RunTest(const FString &Parameters)
{
    // 真の M_ref (壁の LED の原色がカメラにどう写るか。混色の漏れあり)
    const FMatrix3x3 TrueM = FMatrix3x3::FromColumns(FVector3d(0.80, 0.10, 0.02), FVector3d(0.15, 0.90, 0.08), FVector3d(0.03, 0.05, 0.85));
    const FVector3d ProbeBlack(0.002, 0.002, 0.003);   // 壁の黒もプローブにはわずかに見える
    const FVector3d CameraBlack(0.01, 0.01, 0.01);

    TArray<FLightSyncCalibrationStep> Steps = ULightSyncCalibrator::MakeWallSteps();
    for (FLightSyncCalibrationStep &Step : Steps)
    {
        // プローブが測る値 (トーンカーブ後の壁の光) は色ごとに違ってよい
        const FVector3d T = FVector3d(Step.PatchColor.R * 0.62, Step.PatchColor.G * 0.71, Step.PatchColor.B * 0.55) + ProbeBlack;
        Step.ProbeReference = ToColor(T);
        Step.CameraCode = ToColor(TrueM.Transform(T - ProbeBlack) + CameraBlack);
        Step.bMeasured = true;
    }

    FLinearColor R, G, B;
    float Gamma = 0.0f;
    FString Report;
    TestTrue(TEXT("fit ok"), ULightSyncCalibrator::FitStageFromSteps(Steps, ELightSyncCameraTransfer::Linear, R, G, B, Gamma, Report));
    TestTrue(FString::Printf(TEXT("linear wall → gamma %.4f"), Gamma), FMath::IsNearlyEqual(Gamma, 1.0f, 0.01f));
    AddInfo(Report);
    TestTrue(TEXT("column R"), Near(R, TrueM.GetColumn(0), 1e-4));
    TestTrue(TEXT("column G"), Near(G, TrueM.GetColumn(1), 1e-4));
    TestTrue(TEXT("column B"), Near(B, TrueM.GetColumn(2), 1e-4));
    return true;
}

LS_CAL_TEST(FLightSyncCalRoundTripTest, "CalibratedFixtureMatchesWallInCamera")
bool FLightSyncCalRoundTripTest::RunTest(const FString &Parameters)
{
    // 校正した灯体 + M_ref で解くと、灯体の光がカメラ上で壁と同じ色になる (= この仕組みの目的)
    const FLightSyncFixtureProfileData Base = LightSyncFixture::MakePreset(ELightSyncColorMode::RGBW);
    const TArray<FLightSyncCalibrationStep> Steps = MeasureFixture(Base, [](double L) { return FMath::Pow(L, 2.2); },
                                                                   [](const FVector3d &V) { return ToColor(V); });
    FLightSyncFixtureProfileData Fitted;
    FString Report;
    ULightSyncCalibrator::FitFixtureFromSteps(Base, Steps, ELightSyncCameraTransfer::Linear, Fitted, Report);

    const FMatrix3x3 Ref = FMatrix3x3::FromColumns(FVector3d(0.80, 0.10, 0.02), FVector3d(0.15, 0.90, 0.08), FVector3d(0.03, 0.05, 0.85));
    const FLinearColor Target(0.30f, 0.25f, 0.18f);
    const FLightSyncFixtureOutput Out = LightSyncFixture::Compute(Fitted, Target, Ref, 1.0);

    // 真の灯体に送った DMX 値をカメラで見た色を再現
    const TArray<FVector3d> Truth = {TrueR, TrueG, TrueB, TrueW};
    FVector3d Camera = FVector3d::ZeroVector;
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const double Level = Out.Values[Index] / 255.0;            // Dimmer なし: 各エミッタにカーブの逆を適用済み
        Camera += Truth[Index] * FMath::Pow(Level, 2.2);           // 真の調光カーブで光になる
    }
    const FVector3d Wanted = Ref.Transform(FVector3d(Target.R, Target.G, Target.B));
    const double Error = (Camera - Wanted).Size() / Wanted.Size();
    TestTrue(FString::Printf(TEXT("camera error %.3f%% (8bit 量子化のみ)"), Error * 100.0), Error < 0.01);
    TestFalse(TEXT("in gamut"), Out.bOutOfGamut);
    return true;
}

LS_CAL_TEST(FLightSyncCalWallCurveTest, "StageFitRecoversWallResponseCurve")
bool FLightSyncCalWallCurveTest::RunTest(const FString &Parameters)
{
    // 壁の実際の応答が想定 (プローブのデコード) より 1.2 乗だけ強い場合: 真の光 = T^1.2
    const double TrueGamma = 1.2;
    const FMatrix3x3 TrueM = FMatrix3x3::FromColumns(FVector3d(0.80, 0.10, 0.02), FVector3d(0.15, 0.90, 0.08), FVector3d(0.03, 0.05, 0.85));
    const FVector3d ProbeBlack(0.001, 0.001, 0.001);
    const FVector3d CameraBlack(0.01, 0.012, 0.011);
    auto Pow3 = [](const FVector3d &V, double G) { return FVector3d(FMath::Pow(V.X, G), FMath::Pow(V.Y, G), FMath::Pow(V.Z, G)); };

    TArray<FLightSyncCalibrationStep> Steps = ULightSyncCalibrator::MakeWallSteps();
    for (FLightSyncCalibrationStep &Step : Steps)
    {
        // プローブが測る値 (トーンカーブ後) は色・明るさで非線形でよい
        const FVector3d T = FVector3d(FMath::Pow(Step.PatchColor.R * 0.7, 0.8), FMath::Pow(Step.PatchColor.G * 0.7, 0.8), FMath::Pow(Step.PatchColor.B * 0.7, 0.8)) + ProbeBlack;
        Step.ProbeReference = ToColor(T);
        Step.CameraCode = ToColor(TrueM.Transform(Pow3(T, TrueGamma) - Pow3(ProbeBlack, TrueGamma)) + CameraBlack);
        Step.bMeasured = true;
    }

    FLinearColor R, G, B;
    float Gamma = 0.0f;
    FString Report;
    TestTrue(TEXT("fit ok"), ULightSyncCalibrator::FitStageFromSteps(Steps, ELightSyncCameraTransfer::Linear, R, G, B, Gamma, Report));
    AddInfo(Report);
    TestTrue(FString::Printf(TEXT("gamma %.4f (1.2)"), Gamma), FMath::IsNearlyEqual(Gamma, 1.2f, 0.005f));
    TestTrue(TEXT("column R"), Near(R, TrueM.GetColumn(0), 2e-3));
    TestTrue(TEXT("column G"), Near(G, TrueM.GetColumn(1), 2e-3));
    TestTrue(TEXT("column B"), Near(B, TrueM.GetColumn(2), 2e-3));

    // 記録時に既に補正ガンマ 1.2 が掛かっていた (再キャリブレーション) 場合も、二重に掛けず 1.2 に戻る
    for (FLightSyncCalibrationStep &Step : Steps)
    {
        const FVector3d T(Step.ProbeReference.R, Step.ProbeReference.G, Step.ProbeReference.B);
        Step.ProbeReference = ToColor(Pow3(T, 1.2));
        Step.ReferenceGammaAtRecord = 1.2f;
    }
    float Gamma2 = 0.0f;
    ULightSyncCalibrator::FitStageFromSteps(Steps, ELightSyncCameraTransfer::Linear, R, G, B, Gamma2, Report);
    TestTrue(FString::Printf(TEXT("recalibration gamma %.4f (1.2)"), Gamma2), FMath::IsNearlyEqual(Gamma2, 1.2f, 0.005f));
    return true;
}

LS_CAL_TEST(FLightSyncCalWallSceneModeTest, "SceneModeWallCalibrationKeepsGammaOne")
bool FLightSyncCalWallSceneModeTest::RunTest(const FString &Parameters)
{
    TArray<FLightSyncCalibrationStep> Steps = ULightSyncCalibrator::MakeWallSteps();
    for (FLightSyncCalibrationStep &Step : Steps)
    {
        Step.ProbeReference = Step.PatchColor;
        Step.CameraCode = FLinearColor(FMath::Pow(Step.PatchColor.R, 1.5f), FMath::Pow(Step.PatchColor.G, 1.5f), FMath::Pow(Step.PatchColor.B, 1.5f));
        Step.bRecordedInWallMode = false;
        Step.bMeasured = true;
    }
    FLinearColor R, G, B;
    float Gamma = 0.0f;
    FString Report;
    ULightSyncCalibrator::FitStageFromSteps(Steps, ELightSyncCameraTransfer::Linear, R, G, B, Gamma, Report);
    TestTrue(TEXT("gamma stays 1 in Scene mode"), Gamma == 1.0f);
    TestTrue(TEXT("warns about Scene mode"), Report.Contains(TEXT("Scene")));
    return true;
}

#undef LS_CAL_TEST

#endif // WITH_DEV_AUTOMATION_TESTS
