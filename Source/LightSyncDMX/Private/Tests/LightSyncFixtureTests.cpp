// Copyright UE-Comp. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "LightSyncFixtureProfile.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace LightSyncFixtureTests
{
    const LightSyncColor::FMatrix3x3 Identity = LightSyncColor::FMatrix3x3::Identity();

    FString Join(const TArray<uint8> &Values)
    {
        FString Out;
        for (uint8 V : Values)
        {
            Out += FString::Printf(TEXT("%d "), V);
        }
        return Out.TrimEnd();
    }

    uint8 Byte(double Level)
    {
        return static_cast<uint8>(FMath::RoundToInt(FMath::Clamp(Level, 0.0, 1.0) * 255.0));
    }
}

using namespace LightSyncFixtureTests;

#define LS_FIXTURE_TEST(Class, Name) \
    IMPLEMENT_SIMPLE_AUTOMATION_TEST(Class, "LightSync.Fixture." Name, \
        EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

LS_FIXTURE_TEST(FLightSyncFixtureRGBTest, "RGBPresetIsIdentity")
bool FLightSyncFixtureRGBTest::RunTest(const FString &Parameters)
{
    const FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::RGB);
    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, FLinearColor(0.2f, 0.5f, 0.8f), Identity, 1.0);
    const TArray<uint8> Expected = {Byte(0.2), Byte(0.5), Byte(0.8)};
    TestEqual(FString::Printf(TEXT("values %s"), *Join(O.Values)), O.Values, Expected);
    TestFalse(TEXT("not clipped"), O.bClipped);
    TestFalse(TEXT("in gamut"), O.bOutOfGamut);
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixtureClipTest, "OverRangeKeepsRatios")
bool FLightSyncFixtureClipTest::RunTest(const FString &Parameters)
{
    const FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::RGB);
    // 1.0 を超える目標は色比を保って下げる (チャンネルごとのクランプで色相が変わらないこと)
    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, FLinearColor(4.0f, 2.0f, 1.0f), Identity, 1.0);
    const TArray<uint8> Expected = {255, Byte(0.5), Byte(0.25)};
    TestEqual(FString::Printf(TEXT("values %s"), *Join(O.Values)), O.Values, Expected);
    TestTrue(TEXT("clipped flag"), O.bClipped);
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixtureRGBWTest, "RGBWUsesWhiteAndStaysExact")
bool FLightSyncFixtureRGBWTest::RunTest(const FString &Parameters)
{
    const FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::RGBW);
    const FLinearColor Target(0.6f, 0.5f, 0.4f);
    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, Target, Identity, 1.0);

    TestEqual(TEXT("4 emitters"), O.EmitterOutputs.Num(), 4);
    if (O.EmitterOutputs.Num() == 4)
    {
        TestTrue(FString::Printf(TEXT("white used (%.3f)"), O.EmitterOutputs[3]), O.EmitterOutputs[3] > 0.2);
        // 再構成した色が目標と一致 (W = (1,1,1))
        const double R = O.EmitterOutputs[0] + O.EmitterOutputs[3];
        const double G = O.EmitterOutputs[1] + O.EmitterOutputs[3];
        const double B = O.EmitterOutputs[2] + O.EmitterOutputs[3];
        TestTrue(FString::Printf(TEXT("reconstructed %.4f %.4f %.4f"), R, G, B),
                 FMath::IsNearlyEqual(R, 0.6, 1e-4) && FMath::IsNearlyEqual(G, 0.5, 1e-4) && FMath::IsNearlyEqual(B, 0.4, 1e-4));
    }
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixtureDimmerTest, "DimmerCarriesIntensity")
bool FLightSyncFixtureDimmerTest::RunTest(const FString &Parameters)
{
    const FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::DRGB);
    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, FLinearColor(0.25f, 0.5f, 0.125f), Identity, 1.0);
    // Dimmer = 最大出力 0.5、エミッタは比率 (1, 0.5, 0.25 を正規化 → 0.5, 1, 0.25)
    const TArray<uint8> Expected = {Byte(0.5), Byte(0.5), 255, Byte(0.25)};
    TestEqual(FString::Printf(TEXT("values %s"), *Join(O.Values)), O.Values, Expected);
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixtureGammaTest, "GammaCurveIsInverted")
bool FLightSyncFixtureGammaTest::RunTest(const FString &Parameters)
{
    FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::RGB);
    P.DimmingCurve = ELightSyncDimmingCurve::Gamma;
    P.DimmingGamma = 2.2f;
    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, FLinearColor(0.5f, 0.5f, 0.5f), Identity, 1.0);
    // 光出力 0.5 を出すレベル = 0.5^(1/2.2)
    const uint8 Expected = Byte(FMath::Pow(0.5, 1.0 / 2.2));
    TestEqual(FString::Printf(TEXT("R=%d"), O.Values[0]), O.Values[0], Expected);
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixtureCCTTest, "CCTPresetMapsKelvinAndIntensity")
bool FLightSyncFixtureCCTTest::RunTest(const FString &Parameters)
{
    const FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::CCT);

    // Planck 3000K の色度、輝度 0.4 の目標 (0.5 だと 127.5 で丸めの境界になるので避ける)
    const FVector2d xy = LightSyncColor::CCTDuvToxy(3000.0, 0.0);
    const FVector3d XYZ = LightSyncColor::xyYToXYZ(FVector3d(xy.X, xy.Y, 0.4));
    const FVector3d RGB = LightSyncColor::XYZToWorkingRGB(XYZ);
    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, FLinearColor(RGB.X, RGB.Y, RGB.Z), Identity, 1.0);

    TestTrue(FString::Printf(TEXT("CCT %.1f K"), O.CCT), FMath::Abs(O.CCT - 3000.0) < 2.0);
    TestTrue(FString::Printf(TEXT("Duv %.5f"), O.Duv), FMath::Abs(O.Duv) < 1e-4);
    TestEqual(TEXT("CCT channel"), O.Values[0], Byte((3000.0 - 2700.0) / (6500.0 - 2700.0)));
    TestTrue(FString::Printf(TEXT("intensity %.5f (0.4)"), O.Intensity), FMath::Abs(O.Intensity - 0.4) < 1e-3);
    TestEqual(TEXT("Dimmer channel"), O.Values[1], Byte(0.4));
    TestFalse(TEXT("in range"), O.bOutOfGamut);

    // 範囲外の色温度 (10000K) はクランプして色域外フラグ
    const FVector2d xyHigh = LightSyncColor::CCTDuvToxy(10000.0, 0.0);
    const FVector3d RGBHigh = LightSyncColor::XYZToWorkingRGB(LightSyncColor::xyYToXYZ(FVector3d(xyHigh.X, xyHigh.Y, 0.3)));
    const FLightSyncFixtureOutput OHigh = LightSyncFixture::Compute(P, FLinearColor(RGBHigh.X, RGBHigh.Y, RGBHigh.Z), Identity, 1.0);
    TestEqual(TEXT("CCT clamped to max"), OHigh.Values[0], static_cast<uint8>(255));
    TestTrue(TEXT("out of range flagged"), OHigh.bOutOfGamut);
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixture10chTest, "TenChannelLayout")
bool FLightSyncFixture10chTest::RunTest(const FString &Parameters)
{
    const FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::CCTRGBW10CH);
    TestEqual(TEXT("10 channels"), P.GetChannelCount(), 10);

    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, FLinearColor(0.5f, 0.0f, 0.0f), Identity, 1.0);
    TestEqual(TEXT("10 values"), O.Values.Num(), 10);
    if (O.Values.Num() == 10)
    {
        TestEqual(TEXT("Dimmer = 0.5"), O.Values[2], Byte(0.5));
        TestEqual(TEXT("Crossfade fixed 255"), O.Values[3], static_cast<uint8>(255));
        TestEqual(TEXT("R full (ratio)"), O.Values[4], static_cast<uint8>(255));
        TestEqual(TEXT("G off"), O.Values[5], static_cast<uint8>(0));
        TestEqual(TEXT("W off for pure red"), O.Values[7], static_cast<uint8>(0));
        TestEqual(TEXT("Strobe fixed 0"), O.Values[8], static_cast<uint8>(0));
    }
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixture16BitTest, "SixteenBitChannels")
bool FLightSyncFixture16BitTest::RunTest(const FString &Parameters)
{
    FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::RGB);
    for (FLightSyncFixtureChannel &Channel : P.Channels)
    {
        Channel.b16Bit = true;
    }
    TestEqual(TEXT("6 channels"), P.GetChannelCount(), 6);

    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, FLinearColor(0.001f, 0.5f, 1.0f), Identity, 1.0);
    auto Decode = [&O](int32 Index) -> int32 { return (static_cast<int32>(O.Values[Index * 2]) << 8) | O.Values[Index * 2 + 1]; };
    TestEqual(TEXT("R 16bit"), Decode(0), static_cast<int32>(FMath::RoundToInt(0.001 * 65535.0)));
    TestEqual(TEXT("G 16bit"), Decode(1), static_cast<int32>(FMath::RoundToInt(0.5 * 65535.0)));
    TestEqual(TEXT("B 16bit"), Decode(2), 65535);
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixtureCalibrationTest, "StageCalibrationAndFixtureResponse")
bool FLightSyncFixtureCalibrationTest::RunTest(const FString &Parameters)
{
    // 灯体の R エミッタが少し緑を含む (カメラで見た応答)。目標の純赤を出すには G を減らす必要がある
    FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::RGB);
    P.Emitters[0].CameraResponse = FLinearColor(1.0f, 0.1f, 0.0f);
    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, FLinearColor(0.5f, 0.5f, 0.0f), Identity, 1.0);
    // R=0.5 → カメラ G に 0.05 加わるので G エミッタは 0.45
    TestEqual(FString::Printf(TEXT("R %d"), O.Values[0]), O.Values[0], Byte(0.5));
    TestEqual(FString::Printf(TEXT("G %d"), O.Values[1]), O.Values[1], Byte(0.45));

    // 基準 → カメラの行列 (カメラが基準の 2 倍明るく見る) は強さに効く
    const LightSyncColor::FMatrix3x3 Ref = LightSyncColor::FMatrix3x3::FromColumns(FVector3d(2, 0, 0), FVector3d(0, 2, 0), FVector3d(0, 0, 2));
    const FLightSyncFixtureProfileData RGB = LightSyncFixture::MakePreset(ELightSyncColorMode::RGB);
    const FLightSyncFixtureOutput O2 = LightSyncFixture::Compute(RGB, FLinearColor(0.2f, 0.1f, 0.3f), Ref, 1.0);
    const TArray<uint8> Expected = {Byte(0.4), Byte(0.2), Byte(0.6)};
    TestEqual(FString::Printf(TEXT("calibrated %s"), *Join(O2.Values)), O2.Values, Expected);
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixtureBlackoutTest, "BlackoutKeepsFixedValues")
bool FLightSyncFixtureBlackoutTest::RunTest(const FString &Parameters)
{
    const FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::CCTRGBW10CH);
    const FLightSyncFixtureOutput O = LightSyncFixture::ComputeBlackout(P);
    const TArray<uint8> Expected = {0, 0, 0, 255, 0, 0, 0, 0, 0, 0};
    TestEqual(FString::Printf(TEXT("blackout %s"), *Join(O.Values)), O.Values, Expected);
    return true;
}

LS_FIXTURE_TEST(FLightSyncFixtureNanliteTest, "NanlitePavoSlimLayout")
bool FLightSyncFixtureNanliteTest::RunTest(const FString &Parameters)
{
    // Nanlite PavoSlim「CCT & RGBW 8bit」: Dimmer / CCT / G-M / Crossfade / R / G / B / W / Strobe / Reserved / Reserved
    const FLightSyncFixtureProfileData P = LightSyncFixture::MakePreset(ELightSyncColorMode::NanlitePavoSlimRGBW11CH);
    TestEqual(TEXT("11 channels"), P.GetChannelCount(), 11);

    const FLightSyncFixtureOutput O = LightSyncFixture::Compute(P, FLinearColor(0.5f, 0.0f, 0.0f), Identity, 1.0);
    if (TestEqual(TEXT("11 values"), O.Values.Num(), 11))
    {
        TestEqual(TEXT("ch1 Dimmer = 0.5"), O.Values[0], Byte(0.5));
        TestEqual(TEXT("ch3 G/M neutral"), O.Values[2], static_cast<uint8>(132));
        TestEqual(TEXT("ch4 Crossfade RGBW"), O.Values[3], static_cast<uint8>(255));
        TestEqual(TEXT("ch5 R full (ratio)"), O.Values[4], static_cast<uint8>(255));
        TestEqual(TEXT("ch6 G off"), O.Values[5], static_cast<uint8>(0));
        TestEqual(TEXT("ch8 W off for pure red"), O.Values[7], static_cast<uint8>(0));
        TestEqual(TEXT("ch9 Strobe off"), O.Values[8], static_cast<uint8>(0));
    }

    // 消灯でもモード切替 (Crossfade) と G/M は保つ
    const FLightSyncFixtureOutput B = LightSyncFixture::ComputeBlackout(P);
    const TArray<uint8> Expected = {0, 0, 132, 255, 0, 0, 0, 0, 0, 0, 0};
    TestEqual(FString::Printf(TEXT("blackout %s"), *Join(B.Values)), B.Values, Expected);
    return true;
}

#undef LS_FIXTURE_TEST

#endif // WITH_DEV_AUTOMATION_TESTS
