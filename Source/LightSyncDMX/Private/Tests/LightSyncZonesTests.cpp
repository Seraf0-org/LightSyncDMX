// Copyright UE-Comp. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "LightSyncZones.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace LightSyncZonesTests
{
    constexpr int32 TestCubeSize = 64;
    constexpr float Tolerance = 0.005f; // 0.5% (離散化誤差)

    /** 方向 → 放射輝度 の関数からキューブを作る */
    template <typename FuncType>
    void MakeCube(TArray<FLinearColor> (&Faces)[6], FuncType Radiance)
    {
        for (int32 Face = 0; Face < 6; ++Face)
        {
            Faces[Face].SetNumUninitialized(TestCubeSize * TestCubeSize);
            for (int32 Y = 0; Y < TestCubeSize; ++Y)
            {
                for (int32 X = 0; X < TestCubeSize; ++X)
                {
                    const FVector3f Dir = LightSyncZones::FaceUVToDirection(Face, (X + 0.5f) / TestCubeSize, (Y + 0.5f) / TestCubeSize).GetSafeNormal();
                    Faces[Face][Y * TestCubeSize + X] = Radiance(Dir);
                }
            }
        }
    }

    FLightSyncZone MakeZone(FName Name, ELightSyncZoneShape Shape, const FVector &Dir, bool bShared, float Cone = 30.0f, float Soft = 10.0f)
    {
        FLightSyncZone Zone;
        Zone.Name = Name;
        Zone.Shape = Shape;
        Zone.Direction = Dir;
        Zone.bShareDirections = bShared;
        Zone.ConeHalfAngle = Cone;
        Zone.EdgeSoftness = Soft;
        return Zone;
    }

    LightSyncZones::FResult Run(const TArray<FLinearColor> (&Faces)[6], const TArray<FLightSyncZone> &Zones, const FQuat &Rotation = FQuat::Identity)
    {
        const LightSyncZones::FSetup Setup = LightSyncZones::BuildSetup(Zones, Rotation, 4.0f, FVector3f(0.2126f, 0.7152f, 0.0722f));
        return LightSyncZones::IntegrateCube(Faces, TestCubeSize, Setup);
    }

    bool NearlyEqualRel(float A, float B, float Tol)
    {
        return FMath::Abs(A - B) <= Tol * FMath::Max(1.0f, FMath::Abs(B));
    }
}

using namespace LightSyncZonesTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesSolidAngleTest, "LightSync.Zones.SolidAngleSumsTo4Pi",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesSolidAngleTest::RunTest(const FString &Parameters)
{
    double Sum = 0.0;
    for (int32 Face = 0; Face < 6; ++Face)
    {
        for (int32 Y = 0; Y < TestCubeSize; ++Y)
        {
            for (int32 X = 0; X < TestCubeSize; ++X)
            {
                Sum += LightSyncZones::TexelSolidAngle((X + 0.5f) / TestCubeSize, (Y + 0.5f) / TestCubeSize, TestCubeSize);
            }
        }
    }
    TestTrue(FString::Printf(TEXT("Σ dΩ = %.6f (4π = %.6f)"), Sum, 4.0 * UE_DOUBLE_PI),
             FMath::Abs(Sum - 4.0 * UE_DOUBLE_PI) / (4.0 * UE_DOUBLE_PI) < 1e-3);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesUniformTest, "LightSync.Zones.UniformEnvironmentGivesOne",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesUniformTest::RunTest(const FString &Parameters)
{
    TArray<FLinearColor> Faces[6];
    MakeCube(Faces, [](const FVector3f &) { return FLinearColor(0.5f, 1.0f, 2.0f); });

    TArray<FLightSyncZone> Zones;
    Zones.Add(MakeZone("Sphere", ELightSyncZoneShape::Sphere, FVector::UpVector, false));
    Zones.Add(MakeZone("Irr", ELightSyncZoneShape::Irradiance, FVector(1, 1, 0), false));
    Zones.Add(MakeZone("Ring", ELightSyncZoneShape::Ring, FVector::UpVector, false));

    const LightSyncZones::FResult R = Run(Faces, Zones);
    for (int32 Z = 0; Z < 3; ++Z)
    {
        // 一様な放射輝度 L の環境ではどの形状も L を返す (色比も保つ)
        TestTrue(FString::Printf(TEXT("%s R"), *Zones[Z].Name.ToString()), NearlyEqualRel(R.Zones[Z].X, 0.5f, Tolerance));
        TestTrue(FString::Printf(TEXT("%s G"), *Zones[Z].Name.ToString()), NearlyEqualRel(R.Zones[Z].Y, 1.0f, Tolerance));
        TestTrue(FString::Printf(TEXT("%s B"), *Zones[Z].Name.ToString()), NearlyEqualRel(R.Zones[Z].Z, 2.0f, Tolerance));
    }
    TestTrue(TEXT("Sphere coverage = 4π"), NearlyEqualRel(R.Zones[0].W, 4.0f * UE_PI, Tolerance));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesHemisphereTest, "LightSync.Zones.UpperHemisphereOnly",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesHemisphereTest::RunTest(const FString &Parameters)
{
    TArray<FLinearColor> Faces[6];
    MakeCube(Faces, [](const FVector3f &Dir) { return Dir.Z > 0.0f ? FLinearColor::White : FLinearColor::Black; });

    TArray<FLightSyncZone> Zones;
    Zones.Add(MakeZone("Up", ELightSyncZoneShape::Irradiance, FVector::UpVector, false));
    Zones.Add(MakeZone("Down", ELightSyncZoneShape::Irradiance, FVector::DownVector, false));
    Zones.Add(MakeZone("Sphere", ELightSyncZoneShape::Sphere, FVector::UpVector, false));
    Zones.Add(MakeZone("Ring", ELightSyncZoneShape::Ring, FVector::UpVector, false));
    Zones.Add(MakeZone("Side", ELightSyncZoneShape::Irradiance, FVector::ForwardVector, false));

    const LightSyncZones::FResult R = Run(Faces, Zones);
    TestTrue(FString::Printf(TEXT("Up = %.4f (1)"), R.Zones[0].X), NearlyEqualRel(R.Zones[0].X, 1.0f, Tolerance));
    TestTrue(FString::Printf(TEXT("Down = %.4f (0)"), R.Zones[1].X), FMath::Abs(R.Zones[1].X) < Tolerance);
    TestTrue(FString::Printf(TEXT("Sphere = %.4f (0.5)"), R.Zones[2].X), NearlyEqualRel(R.Zones[2].X, 0.5f, Tolerance));
    TestTrue(FString::Printf(TEXT("Ring = %.4f (0.5)"), R.Zones[3].X), NearlyEqualRel(R.Zones[3].X, 0.5f, Tolerance));
    // 垂直な面 (+X 向き) が上半球から受ける照度 / π = 1/2
    TestTrue(FString::Printf(TEXT("Side = %.4f (0.5)"), R.Zones[4].X), NearlyEqualRel(R.Zones[4].X, 0.5f, Tolerance));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesPartitionTest, "LightSync.Zones.PartitionNoDoubleCounting",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesPartitionTest::RunTest(const FString &Parameters)
{
    TArray<FLinearColor> Faces[6];
    MakeCube(Faces, [](const FVector3f &) { return FLinearColor::White; });

    // 既定構成 (Top: 上向き半球 + Side: 水平リング) は方向を分け合う
    TArray<FLightSyncZone> Zones;
    Zones.Add(MakeZone("Top", ELightSyncZoneShape::Irradiance, FVector::UpVector, true));
    Zones.Add(MakeZone("Side", ELightSyncZoneShape::Ring, FVector::UpVector, true));
    Zones.Add(MakeZone("Bottom", ELightSyncZoneShape::Irradiance, FVector::DownVector, true));

    const LightSyncZones::FResult R = Run(Faces, Zones);
    const float CoverageSum = R.Zones[0].W + R.Zones[1].W + R.Zones[2].W;
    TestTrue(FString::Printf(TEXT("Σ coverage = %.4f (4π)"), CoverageSum), NearlyEqualRel(CoverageSum, 4.0f * UE_PI, Tolerance));

    // 上下対称なので Top と Bottom は等しく、どれも 1 未満 (分け合っている)
    TestTrue(TEXT("Top == Bottom"), NearlyEqualRel(R.Zones[0].X, R.Zones[2].X, Tolerance));
    TestTrue(TEXT("Top < 1"), R.Zones[0].X < 1.0f - Tolerance);
    TestTrue(TEXT("Side < 1"), R.Zones[1].X < 1.0f - Tolerance);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesExcludeTest, "LightSync.Zones.ExcludeRemovesDirections",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesExcludeTest::RunTest(const FString &Parameters)
{
    TArray<FLinearColor> Faces[6];
    MakeCube(Faces, [](const FVector3f &) { return FLinearColor::White; });

    TArray<FLightSyncZone> Zones;
    Zones.Add(MakeZone("Wall", ELightSyncZoneShape::Exclude, FVector::ForwardVector, false, 90.0f, 0.5f));
    Zones.Add(MakeZone("Sphere", ELightSyncZoneShape::Sphere, FVector::UpVector, false));
    Zones.Add(MakeZone("Front", ELightSyncZoneShape::Irradiance, FVector::ForwardVector, false));
    Zones.Add(MakeZone("Back", ELightSyncZoneShape::Irradiance, FVector::BackwardVector, false));

    const LightSyncZones::FResult R = Run(Faces, Zones);
    TestTrue(FString::Printf(TEXT("Sphere = %.4f (0.5)"), R.Zones[1].X), NearlyEqualRel(R.Zones[1].X, 0.5f, 0.01f));
    TestTrue(FString::Printf(TEXT("Front = %.4f (0)"), R.Zones[2].X), FMath::Abs(R.Zones[2].X) < 0.01f);
    TestTrue(FString::Printf(TEXT("Back = %.4f (1)"), R.Zones[3].X), NearlyEqualRel(R.Zones[3].X, 1.0f, 0.01f));
    TestTrue(TEXT("Exclude zone outputs nothing"), R.Zones[0].X == 0.0f && R.Zones[0].W == 0.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesRotationTest, "LightSync.Zones.ProbeRotationAppliesToZones",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesRotationTest::RunTest(const FString &Parameters)
{
    TArray<FLinearColor> Faces[6];
    MakeCube(Faces, [](const FVector3f &Dir) { return Dir.X > 0.0f ? FLinearColor::White : FLinearColor::Black; });

    TArray<FLightSyncZone> Zones;
    Zones.Add(MakeZone("LocalUp", ELightSyncZoneShape::Irradiance, FVector::UpVector, false));

    // ローカル +Z をワールド +X に向ける回転
    const FQuat Rotation = FQuat::FindBetweenNormals(FVector::UpVector, FVector::ForwardVector);
    const LightSyncZones::FResult Rotated = Run(Faces, Zones, Rotation);
    const LightSyncZones::FResult Unrotated = Run(Faces, Zones);
    TestTrue(FString::Printf(TEXT("rotated = %.4f (1)"), Rotated.Zones[0].X), NearlyEqualRel(Rotated.Zones[0].X, 1.0f, Tolerance));
    TestTrue(FString::Printf(TEXT("unrotated = %.4f (0.5)"), Unrotated.Zones[0].X), NearlyEqualRel(Unrotated.Zones[0].X, 0.5f, Tolerance));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesKeyTest, "LightSync.Zones.KeyFindsBrightestDirection",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesKeyTest::RunTest(const FString &Parameters)
{
    const FVector3f SpotDir = FVector3f(0.3f, -0.8f, 0.5f).GetSafeNormal();
    const float SpotCos = FMath::Cos(FMath::DegreesToRadians(10.0f));

    TArray<FLinearColor> Faces[6];
    MakeCube(Faces, [&](const FVector3f &Dir)
    {
        return FVector3f::DotProduct(Dir, SpotDir) > SpotCos ? FLinearColor(20.0f, 10.0f, 5.0f) : FLinearColor(0.1f, 0.1f, 0.1f);
    });

    TArray<FLightSyncZone> Zones;
    Zones.Add(MakeZone("Key", ELightSyncZoneShape::Key, FVector::UpVector, false, 20.0f, 4.0f));
    Zones.Add(MakeZone("Cone", ELightSyncZoneShape::Cone, FVector(SpotDir), false, 20.0f, 4.0f));

    const LightSyncZones::FResult R = Run(Faces, Zones);
    const float AngleDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector3f::DotProduct(R.KeyDirection, SpotDir), -1.0f, 1.0f)));
    TestTrue(FString::Printf(TEXT("Key direction error %.2f deg"), AngleDeg), AngleDeg < 3.0f);
    TestTrue(FString::Printf(TEXT("Key (%.4f) ≈ Cone at true direction (%.4f)"), R.Zones[0].X, R.Zones[1].X),
             NearlyEqualRel(R.Zones[0].X, R.Zones[1].X, 0.02f));
    // 色比はスポットの色 (4:2:1) に近い
    TestTrue(TEXT("Key hue preserved"), NearlyEqualRel(R.Zones[0].X / R.Zones[0].Z, 4.0f, 0.1f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesTransferTest, "LightSync.Zones.DecodesDisplayTransferPerTexel",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesTransferTest::RunTest(const FString &Parameters)
{
    // 半分の方向が信号 1.0、残りが 0.5 の壁信号。EOTF は非線形なので「デコード → 積分」でないと値が変わる
    TArray<FLinearColor> Faces[6];
    MakeCube(Faces, [](const FVector3f &Dir) { return Dir.Z > 0.0f ? FLinearColor(1.0f, 1.0f, 1.0f) : FLinearColor(0.5f, 0.5f, 0.5f); });

    TArray<FLightSyncZone> Zones;
    Zones.Add(MakeZone("Sphere", ELightSyncZoneShape::Sphere, FVector::UpVector, false));

    const float Srgb05 = FMath::Pow((0.5f + 0.055f) / 1.055f, 2.4f); // 0.21404
    const LightSyncZones::FSetup Setup = LightSyncZones::BuildSetup(Zones, FQuat::Identity, 4.0f,
                                                                    FVector3f(0.2126f, 0.7152f, 0.0722f), ELightSyncInputTransfer::SRGB);
    const LightSyncZones::FResult R = LightSyncZones::IntegrateCube(Faces, TestCubeSize, Setup);
    const float Expected = 0.5f * (1.0f + Srgb05);
    TestTrue(FString::Printf(TEXT("sRGB decode: %.5f (expected %.5f)"), R.Zones[0].X, Expected), NearlyEqualRel(R.Zones[0].X, Expected, Tolerance));

    const LightSyncZones::FSetup Setup22 = LightSyncZones::BuildSetup(Zones, FQuat::Identity, 4.0f,
                                                                      FVector3f(0.2126f, 0.7152f, 0.0722f), ELightSyncInputTransfer::Gamma22);
    const float Expected22 = 0.5f * (1.0f + FMath::Pow(0.5f, 2.2f));
    const LightSyncZones::FResult R22 = LightSyncZones::IntegrateCube(Faces, TestCubeSize, Setup22);
    TestTrue(FString::Printf(TEXT("gamma 2.2 decode: %.5f (expected %.5f)"), R22.Zones[0].X, Expected22), NearlyEqualRel(R22.Zones[0].X, Expected22, Tolerance));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesChangeTest, "LightSync.Zones.RelativeChangeDetectsSteps",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesChangeTest::RunTest(const FString &Parameters)
{
    const TArray<FLinearColor> Base = {FLinearColor(0.5f, 0.25f, 0.1f), FLinearColor(0.0f, 0.0f, 0.0f)};

    // 同じ値 → 0
    TestEqual(TEXT("identical"), LightSyncZones::RelativeChange(Base, Base), 0.0f);

    // 明るいチャンネル基準の相対変化: 0.5 → 0.505 は 1%
    TArray<FLinearColor> Step = Base;
    Step[0].R = 0.505f;
    TestTrue(TEXT("1% step"), FMath::IsNearlyEqual(LightSyncZones::RelativeChange(Base, Step), 0.01f, 1e-4f));

    // 暗いチャンネルの小さな変化も、ゾーンの明るさに対する割合で見る (0.1 → 0.105 は 0.5 に対して 1%)
    Step = Base;
    Step[0].B = 0.105f;
    TestTrue(TEXT("dim channel relative to zone"), FMath::IsNearlyEqual(LightSyncZones::RelativeChange(Base, Step), 0.01f, 1e-4f));

    // ほぼ黒のゾーンは下限 (Floor) で割る: 0 → 0.0005 は 1% の下限に対して 5%
    Step = Base;
    Step[1] = FLinearColor(0.0005f, 0.0f, 0.0f);
    TestTrue(TEXT("floor"), FMath::IsNearlyEqual(LightSyncZones::RelativeChange(Base, Step, 0.01f), 0.05f, 1e-4f));

    // 消灯 → 点灯は大きな変化
    Step = Base;
    Step[1] = FLinearColor(1.0f, 1.0f, 1.0f);
    TestTrue(TEXT("off to on"), LightSyncZones::RelativeChange(Base, Step) >= 1.0f);

    // 対称
    TestEqual(TEXT("symmetric"), LightSyncZones::RelativeChange(Step, Base), LightSyncZones::RelativeChange(Base, Step));

    // ゾーン数が違えば変化あり
    TestTrue(TEXT("count mismatch"), LightSyncZones::RelativeChange(Base, {FLinearColor::White}) > 1.0f);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncZonesHDRTransferTest, "LightSync.Zones.DecodesPQAndCustomGamma",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncZonesHDRTransferTest::RunTest(const FString &Parameters)
{
    // SMPTE ST 2084 の既知の値 (コード値 → nits)
    struct FPoint { float Code; float Nits; };
    const FPoint Points[] = {{0.0f, 0.0f}, {0.5080784f, 100.0f}, {0.5806888f, 203.0f}, {0.7518271f, 1000.0f}, {1.0f, 10000.0f}};
    for (const FPoint &P : Points)
    {
        const float Nits = LightSyncZones::PQToNits(P.Code);
        TestTrue(FString::Printf(TEXT("PQ %.4f -> %.3f nits (%.1f)"), P.Code, Nits, P.Nits),
                 FMath::Abs(Nits - P.Nits) <= FMath::Max(0.002f * P.Nits, 1e-4f));
    }

    // 正規化: 1.0 = 基準白
    TestTrue(TEXT("PQ 100 nits = 1.0 (ref 100)"),
             FMath::IsNearlyEqual(LightSyncZones::DecodeTransfer(ELightSyncInputTransfer::PQ, 0.5080784f, 2.2f, 100.0f), 1.0f, 2e-3f));
    TestTrue(TEXT("PQ 1000 nits = 5.0 (ref 200)"),
             FMath::IsNearlyEqual(LightSyncZones::DecodeTransfer(ELightSyncInputTransfer::PQ, 0.7518271f, 2.2f, 200.0f), 5.0f, 1e-2f));

    // 任意ガンマ
    TestTrue(TEXT("gamma 2.35"),
             FMath::IsNearlyEqual(LightSyncZones::DecodeTransfer(ELightSyncInputTransfer::Gamma, 0.5f, 2.35f), FMath::Pow(0.5f, 2.35f), 1e-6f));

    // 積分も texel ごとにデコードする: PQ で 100 nits の一様な環境 → 全ゾーン 1.0
    TArray<FLinearColor> Faces[6];
    MakeCube(Faces, [](const FVector3f &) { return FLinearColor(0.5080784f, 0.5080784f, 0.5080784f); });
    TArray<FLightSyncZone> Zones;
    Zones.Add(MakeZone("Sphere", ELightSyncZoneShape::Sphere, FVector::UpVector, false));
    Zones.Add(MakeZone("Irr", ELightSyncZoneShape::Irradiance, FVector::UpVector, false));
    LightSyncZones::FSetup Setup = LightSyncZones::BuildSetup(Zones, FQuat::Identity, 4.0f, FVector3f(0.2126f, 0.7152f, 0.0722f),
                                                              ELightSyncInputTransfer::PQ);
    Setup.PQReferenceWhiteNits = 100.0f;
    const LightSyncZones::FResult R = LightSyncZones::IntegrateCube(Faces, TestCubeSize, Setup);
    TestTrue(FString::Printf(TEXT("PQ uniform sphere %.4f"), R.Zones[0].X), NearlyEqualRel(R.Zones[0].X, 1.0f, 3e-3f));
    TestTrue(FString::Printf(TEXT("PQ uniform irradiance %.4f"), R.Zones[1].X), NearlyEqualRel(R.Zones[1].X, 1.0f, 3e-3f));

    // 表示の色域: Rec.2020 の白は白、Rec.2020 の純赤は Rec.709 の外 (負の成分を持つ)
    const UE::Color::FColorSpaceTransform ToSRGB(LightSyncZones::GetDisplayColorSpace(ELightSyncDisplayGamut::Rec2020),
                                                 UE::Color::FColorSpace(UE::Color::EColorSpace::sRGB));
    const FLinearColor White = ToSRGB.Apply(FLinearColor(1.0f, 1.0f, 1.0f));
    TestTrue(TEXT("Rec.2020 white stays white"), White.Equals(FLinearColor(1.0f, 1.0f, 1.0f), 1e-3f));
    const FLinearColor Red = ToSRGB.Apply(FLinearColor(1.0f, 0.0f, 0.0f));
    TestTrue(FString::Printf(TEXT("Rec.2020 red in Rec.709: %.3f %.3f %.3f"), Red.R, Red.G, Red.B), Red.R > 1.5f && Red.G < 0.0f);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
