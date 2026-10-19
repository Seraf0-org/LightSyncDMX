// Copyright UE-Comp. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "LightSyncNDisplay.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncNDisplayOverrideTest, "LightSync.NDisplay.OverrideMatchesSceneViewBlend",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncNDisplayOverrideTest::RunTest(const FString &Parameters)
{
    FPostProcessSettings Dest;
    Dest.AutoExposureBias = 1.0f;
    Dest.ColorSaturation = FVector4(1.0, 1.0, 1.0, 1.0);
    Dest.WhiteTint = 0.25f;

    FPostProcessSettings Src;
    Src.bOverride_AutoExposureBias = true;
    Src.AutoExposureBias = 3.0f;
    Src.bOverride_ColorSaturation = true;
    Src.ColorSaturation = FVector4(0.0, 0.5, 1.0, 1.0);
    Src.bOverride_TemperatureType = true;
    Src.TemperatureType = ETemperatureMethod::TEMP_ColorTemperature;
    Src.WhiteTint = 0.9f; // bOverride なし → 変わらない

    // FSceneView::OverridePostProcessSettings と同じ: 数値は重みで補間、enum は置き換え
    LightSyncNDisplay::OverridePostProcess(Dest, Src, 0.5f);

    TestTrue(FString::Printf(TEXT("bias %.3f (2.0)"), Dest.AutoExposureBias), FMath::IsNearlyEqual(Dest.AutoExposureBias, 2.0f));
    TestTrue(TEXT("bias override flag"), Dest.bOverride_AutoExposureBias != 0);
    TestTrue(TEXT("saturation lerp"), Dest.ColorSaturation.Equals(FVector4(0.5, 0.75, 1.0, 1.0)));
    TestTrue(TEXT("enum replaced"), Dest.TemperatureType == ETemperatureMethod::TEMP_ColorTemperature);
    TestTrue(TEXT("non-overridden untouched"), FMath::IsNearlyEqual(Dest.WhiteTint, 0.25f));
    TestTrue(TEXT("non-overridden flag stays off"), Dest.bOverride_WhiteTint == 0);

    // 重み 0 は何もしない
    FPostProcessSettings Before = Dest;
    LightSyncNDisplay::OverridePostProcess(Dest, Src, 0.0f);
    TestTrue(TEXT("weight 0 no-op"), FMath::IsNearlyEqual(Dest.AutoExposureBias, Before.AutoExposureBias));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncNDisplayNoRootTest, "LightSync.NDisplay.MissingRootKeepsScene",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncNDisplayNoRootTest::RunTest(const FString &Parameters)
{
    FPostProcessSettings Scene;
    Scene.bOverride_AutoExposureBias = true;
    Scene.AutoExposureBias = 1.25f;

    FPostProcessSettings Out;
    FString Report;
    TestFalse(TEXT("no root actor"), LightSyncNDisplay::BuildOuterViewportPostProcess(nullptr, TEXT("VP_0"), Scene, Out, Report));
    TestTrue(TEXT("scene kept"), FMath::IsNearlyEqual(Out.AutoExposureBias, 1.25f));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncNDisplayOCIOSelectTest, "LightSync.NDisplay.ViewportOCIOSelection",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncNDisplayOCIOSelectTest::RunTest(const FString &Parameters)
{
    using LightSyncNDisplay::FOCIOProfileDesc;
    TArray<FOCIOProfileDesc> Profiles;
    Profiles.Add({false, {TEXT("VP_0")}});            // 無効なプロファイル
    Profiles.Add({true, {TEXT("VP_1"), TEXT("vp_0")}}); // VP_0 と VP_1 (大文字小文字は無視)
    Profiles.Add({true, {TEXT("VP_1")}});              // 先勝ちなので VP_1 は 1 番

    // 全ビューポートが無効なら、プロファイルがあっても OCIO なし (nDisplay と同じ)
    TestEqual(TEXT("all disabled -> none"), LightSyncNDisplay::SelectViewportOCIO(false, Profiles, TEXT("VP_1")), -2);
    // 無効なプロファイルは飛ばす
    TestEqual(TEXT("VP_0 -> profile 1"), LightSyncNDisplay::SelectViewportOCIO(true, Profiles, TEXT("VP_0")), 1);
    TestEqual(TEXT("VP_1 -> first match"), LightSyncNDisplay::SelectViewportOCIO(true, Profiles, TEXT("VP_1")), 1);
    // 対象外 / ビューポート未指定は全ビューポートの設定
    TestEqual(TEXT("VP_9 -> all viewports"), LightSyncNDisplay::SelectViewportOCIO(true, Profiles, TEXT("VP_9")), -1);
    TestEqual(TEXT("empty id -> all viewports"), LightSyncNDisplay::SelectViewportOCIO(true, Profiles, FString()), -1);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
