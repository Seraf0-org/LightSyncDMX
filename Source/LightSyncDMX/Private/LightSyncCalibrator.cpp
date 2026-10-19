// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncCalibrator.h"
#include "LightSyncCalibrationPatch.h"
#include "LightProbeActor.h"
#include "DMXColorOutputComponent.h"
#include "OSCColorOutputComponent.h"
#include "LightSyncDMXModule.h"

#include "Engine/Canvas.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "TextureResource.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#endif

using namespace LightSyncColor;

namespace
{
    double Luminance(const FVector3d &RGB)
    {
        return 0.2126 * RGB.X + 0.7152 * RGB.Y + 0.0722 * RGB.Z;
    }

    FVector3d ToVec(const FLinearColor &C)
    {
        return FVector3d(C.R, C.G, C.B);
    }

    FLinearColor ToColor(const FVector3d &V)
    {
        return FLinearColor(static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z), 1.0f);
    }

    ECameraTransfer ToCore(ELightSyncCameraTransfer Transfer)
    {
        switch (Transfer)
        {
        case ELightSyncCameraTransfer::SRGB:      return ECameraTransfer::sRGB;
        case ELightSyncCameraTransfer::Rec709:    return ECameraTransfer::Rec709;
        case ELightSyncCameraTransfer::ArriLogC3: return ECameraTransfer::ArriLogC3_EI800;
        case ELightSyncCameraTransfer::ArriLogC4: return ECameraTransfer::ArriLogC4;
        case ELightSyncCameraTransfer::SonySLog3: return ECameraTransfer::SonySLog3;
        default:                                  return ECameraTransfer::Linear;
        }
    }

    const FLightSyncCalibrationStep *FindStep(const TArray<FLightSyncCalibrationStep> &Steps,
                                              TFunctionRef<bool(const FLightSyncCalibrationStep &)> Pred)
    {
        for (const FLightSyncCalibrationStep &Step : Steps)
        {
            if (Step.bMeasured && Pred(Step))
            {
                return &Step;
            }
        }
        return nullptr;
    }
}

// ============================================================
// 手順
// ============================================================

TArray<FLightSyncCalibrationStep> ULightSyncCalibrator::MakeFixtureSteps(const FLightSyncFixtureProfileData &Profile)
{
    TArray<FLightSyncCalibrationStep> Steps;

    FLightSyncCalibrationStep Off;
    Off.Label = TEXT("消灯 (黒レベル)");
    Off.Kind = ELightSyncCalibrationStepKind::FixtureOff;
    Steps.Add(Off);

    if (Profile.ColorModel == ELightSyncFixtureColorModel::Emitters)
    {
        for (int32 Index = 0; Index < Profile.Emitters.Num(); ++Index)
        {
            FLightSyncCalibrationStep Step;
            Step.Label = FString::Printf(TEXT("エミッタ %s 100%%"), *Profile.Emitters[Index].Name.ToString());
            Step.Kind = ELightSyncCalibrationStepKind::FixtureEmitter;
            Step.EmitterIndex = Index;
            Steps.Add(Step);
        }
    }
    else
    {
        FLightSyncCalibrationStep Step;
        Step.Label = TEXT("白 100% (CCT 中央)");
        Step.Kind = ELightSyncCalibrationStepKind::FixtureEmitter;
        Step.EmitterIndex = 0;
        Steps.Add(Step);
    }

    for (float Level : {0.25f, 0.5f, 0.75f, 1.0f})
    {
        FLightSyncCalibrationStep Step;
        Step.Label = FString::Printf(TEXT("全エミッタ %d%% (調光カーブ)"), FMath::RoundToInt(Level * 100.0f));
        Step.Kind = ELightSyncCalibrationStepKind::FixtureAllLevel;
        Step.Level = Level;
        Steps.Add(Step);
    }
    return Steps;
}

TArray<FLightSyncCalibrationStep> ULightSyncCalibrator::MakeWallSteps()
{
    TArray<FLightSyncCalibrationStep> Steps;
    auto Add = [&Steps](const TCHAR *Label, const FLinearColor &Color)
    {
        FLightSyncCalibrationStep Step;
        Step.Label = Label;
        Step.Kind = ELightSyncCalibrationStepKind::WallPatch;
        Step.PatchColor = Color;
        Steps.Add(Step);
    };
    Add(TEXT("壁 黒"), FLinearColor(0.0f, 0.0f, 0.0f));
    Add(TEXT("壁 赤"), FLinearColor(1.0f, 0.0f, 0.0f));
    Add(TEXT("壁 緑"), FLinearColor(0.0f, 1.0f, 0.0f));
    Add(TEXT("壁 青"), FLinearColor(0.0f, 0.0f, 1.0f));
    Add(TEXT("壁 白"), FLinearColor(1.0f, 1.0f, 1.0f));
    return Steps;
}

void ULightSyncCalibrator::BuildSteps()
{
    FLightSyncFixtureProfileData Profile;
    if (!GetFixtureProfile(Profile))
    {
        Profile = LightSyncFixture::MakePreset(ELightSyncColorMode::RGB);
    }
    FixtureSteps = MakeFixtureSteps(Profile);
    WallSteps = MakeWallSteps();
    LastReport.Reset();
}

bool ULightSyncCalibrator::GetFixtureProfile(FLightSyncFixtureProfileData &OutProfile) const
{
    if (!Probe || !Probe->DMXOutput || !Probe->DMXOutput->FixtureMappings.IsValidIndex(FixtureIndex))
    {
        return false;
    }
    OutProfile = UDMXColorOutputComponent::GetMappingProfile(Probe->DMXOutput->FixtureMappings[FixtureIndex]);
    return true;
}

// ============================================================
// テスト表示
// ============================================================

ALightSyncCalibrationPatch *ULightSyncCalibrator::EnsurePatch(const FLinearColor &Color)
{
    if (!Probe || !Probe->GetWorld())
    {
        return nullptr;
    }

    if (!Patch || Patch->GetWorld() != Probe->GetWorld())
    {
        Patch = Probe->GetWorld()->SpawnActor<ALightSyncCalibrationPatch>(Probe->GetActorLocation(), FRotator::ZeroRotator);
#if WITH_EDITOR
        if (Patch)
        {
            Patch->SetActorLabel(TEXT("LightSync Calibration Patch (一時)"));
        }
#endif
    }

    if (Patch)
    {
        Patch->SetActorLocation(Probe->GetActorLocation());
        Patch->PatchColor = Color;
        Patch->ApplyPatch();
        if (!Patch->HasPatchMaterial())
        {
            UE_LOG(LogLightSyncDMX, Error, TEXT("Calibration: パッチ用マテリアルが見つからないため、壁の測定は正しくできません。"));
        }
    }
    return Patch;
}

bool ULightSyncCalibrator::ShowFixtureStep(int32 Index)
{
    FLightSyncFixtureProfileData Profile;
    if (!FixtureSteps.IsValidIndex(Index) || !GetFixtureProfile(Profile))
    {
        return false;
    }

    // プローブの出力を止めて、壁は黒 (灯体だけでグレーカードを照らす)
    Probe->bSuspendOutput = true;
    EnsurePatch(FLinearColor::Black);

    const FLightSyncCalibrationStep &Step = FixtureSteps[Index];
    TArray<double> Levels;
    Levels.SetNumZeroed(FMath::Max(Profile.Emitters.Num(), 1));
    double Dimmer = 0.0;

    switch (Step.Kind)
    {
    case ELightSyncCalibrationStepKind::FixtureEmitter:
        if (Levels.IsValidIndex(Step.EmitterIndex))
        {
            Levels[Step.EmitterIndex] = 1.0;
        }
        Dimmer = 1.0;
        break;
    case ELightSyncCalibrationStepKind::FixtureAllLevel:
    {
        const bool bHasDimmer = Profile.HasDimmer();
        for (double &Level : Levels)
        {
            Level = bHasDimmer ? 1.0 : Step.Level;
        }
        Dimmer = Step.Level;
        break;
    }
    default:
        break;
    }

    const FLightSyncFixtureOutput Pattern = LightSyncFixture::ComputeTestPattern(Profile, Levels, Dimmer);
    Probe->DMXOutput->SendRawToFixture(FixtureIndex, Pattern.Values);
    ActiveFixtureStep = Index;
    ActiveWallStep = INDEX_NONE;
    return true;
}

bool ULightSyncCalibrator::ShowWallStep(int32 Index)
{
    if (!WallSteps.IsValidIndex(Index) || !Probe)
    {
        return false;
    }

    // 灯体は消して、壁にだけパッチを出す
    Probe->bSuspendOutput = true;
    if (Probe->DMXOutput)
    {
        Probe->DMXOutput->Blackout();
    }
    if (!EnsurePatch(WallSteps[Index].PatchColor * PatchIntensity))
    {
        return false;
    }
    ActiveWallStep = Index;
    ActiveFixtureStep = INDEX_NONE;
    return true;
}

void ULightSyncCalibrator::EndTestOutput()
{
    if (Patch)
    {
        Patch->Destroy();
        Patch = nullptr;
    }
    if (Probe)
    {
        Probe->bSuspendOutput = false;
    }
    ActiveWallStep = INDEX_NONE;
    ActiveFixtureStep = INDEX_NONE;
}

// ============================================================
// 測定
// ============================================================

bool ULightSyncCalibrator::CaptureCameraFeed(FLinearColor &OutCameraCode)
{
    UWorld *World = Probe ? Probe->GetWorld() : nullptr;
    if (!CameraFeed || !World)
    {
        return false;
    }

    if (!FeedReadTarget)
    {
        FeedReadTarget = NewObject<UTextureRenderTarget2D>(this);
        FeedReadTarget->RenderTargetFormat = RTF_RGBA32f;
        FeedReadTarget->ClearColor = FLinearColor::Black;
        FeedReadTarget->InitAutoFormat(16, 16);
        FeedReadTarget->UpdateResourceImmediate(true);
    }

    const FVector2D Min(FMath::Clamp(FMath::Min(FeedROIMin.X, FeedROIMax.X), 0.0, 1.0), FMath::Clamp(FMath::Min(FeedROIMin.Y, FeedROIMax.Y), 0.0, 1.0));
    const FVector2D Max(FMath::Clamp(FMath::Max(FeedROIMin.X, FeedROIMax.X), 0.0, 1.0), FMath::Clamp(FMath::Max(FeedROIMin.Y, FeedROIMax.Y), 0.0, 1.0));
    if ((Max - Min).GetMin() <= 0.0)
    {
        return false;
    }

    // ROI を小さなレンダーターゲットに引き伸ばして描き、全画素を平均する
    UCanvas *Canvas = nullptr;
    FVector2D Size;
    FDrawToRenderTargetContext Context;
    UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, FeedReadTarget, Canvas, Size, Context);
    if (Canvas)
    {
        Canvas->K2_DrawTexture(CameraFeed, FVector2D::ZeroVector, Size, Min, Max - Min, FLinearColor::White, BLEND_Opaque);
    }
    UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Context);

    TArray<FLinearColor> Pixels;
    FTextureRenderTargetResource *Resource = FeedReadTarget->GameThread_GetRenderTargetResource();
    if (!Resource || !Resource->ReadLinearColorPixels(Pixels) || Pixels.Num() == 0)
    {
        return false;
    }

    FVector3d Sum = FVector3d::ZeroVector;
    for (const FLinearColor &Pixel : Pixels)
    {
        Sum += ToVec(Pixel);
    }
    FVector3d Mean = Sum / Pixels.Num();

    // sRGB テクスチャとしてデコードされた値はコード値に戻す
    if (bFeedSampledAsSRGB)
    {
        auto Encode = [](double V)
        {
            V = FMath::Max(V, 0.0);
            return V <= 0.0031308 ? 12.92 * V : 1.055 * FMath::Pow(V, 1.0 / 2.4) - 0.055;
        };
        Mean = FVector3d(Encode(Mean.X), Encode(Mean.Y), Encode(Mean.Z));
    }

    OutCameraCode = ToColor(Mean);
    return true;
}

bool ULightSyncCalibrator::RecordFixtureMeasurement(int32 Index, FLinearColor CameraCode)
{
    if (!FixtureSteps.IsValidIndex(Index))
    {
        return false;
    }
    FixtureSteps[Index].CameraCode = CameraCode;
    FixtureSteps[Index].bMeasured = true;
    return true;
}

bool ULightSyncCalibrator::RecordWallMeasurement(int32 Index, FLinearColor CameraCode)
{
    if (!WallSteps.IsValidIndex(Index) || !Probe)
    {
        return false;
    }

    FLightSyncCalibrationStep &Step = WallSteps[Index];

    // プローブの基準値は、そのパッチを壁に出している間にしか取れない
    if (ActiveWallStep == Index)
    {
        FLinearColor Reference;
        if (!Probe->GetZoneColor(ReferenceZone, Reference))
        {
            LastReport = FString::Printf(TEXT("ゾーン '%s' がプローブにありません (Sphere 形状のゾーンを指定してください)"), *ReferenceZone.ToString());
            return false;
        }
        Step.ProbeReference = Reference;
    }
    else if (!Step.bMeasured)
    {
        LastReport = FString::Printf(TEXT("「%s」を先に [表示] してから記録してください"), *Step.Label);
        return false;
    }

    Step.CameraCode = CameraCode;
    Step.bMeasured = true;
    return true;
}

// ============================================================
// フィット
// ============================================================

FVector3d ULightSyncCalibrator::LinearizeCamera(ELightSyncCameraTransfer Transfer, const FLinearColor &Code)
{
    return LinearizeCameraRGB(ToCore(Transfer), ToVec(Code));
}

bool ULightSyncCalibrator::FitFixtureFromSteps(const FLightSyncFixtureProfileData &Base, const TArray<FLightSyncCalibrationStep> &Steps,
                                              ELightSyncCameraTransfer Transfer, FLightSyncFixtureProfileData &OutProfile, FString &OutReport)
{
    OutProfile = Base;
    OutReport.Reset();

    const FLightSyncCalibrationStep *Off = FindStep(Steps, [](const FLightSyncCalibrationStep &S) { return S.Kind == ELightSyncCalibrationStepKind::FixtureOff; });
    if (!Off)
    {
        OutReport = TEXT("「消灯」の測定がありません");
        return false;
    }
    const FVector3d Black = LinearizeCamera(Transfer, Off->CameraCode);

    // --- エミッタの応答 (黒を引く) ---
    const int32 NumEmitters = Base.ColorModel == ELightSyncFixtureColorModel::Emitters ? Base.Emitters.Num() : FMath::Min(Base.Emitters.Num(), 1);
    if (NumEmitters == 0)
    {
        OutReport = TEXT("プロファイルにエミッタがありません");
        return false;
    }

    FVector3d EmitterSum = FVector3d::ZeroVector;
    for (int32 Index = 0; Index < NumEmitters; ++Index)
    {
        const FLightSyncCalibrationStep *Step = FindStep(Steps, [Index](const FLightSyncCalibrationStep &S)
                                                         { return S.Kind == ELightSyncCalibrationStepKind::FixtureEmitter && S.EmitterIndex == Index; });
        if (!Step)
        {
            OutReport = FString::Printf(TEXT("エミッタ %d の測定がありません"), Index);
            return false;
        }
        const FVector3d Response = LinearizeCamera(Transfer, Step->CameraCode) - Black;
        if (Luminance(Response) <= 0.0)
        {
            OutReport = FString::Printf(TEXT("エミッタ %d の測定値が黒以下です (露出や ROI を確認)"), Index);
            return false;
        }
        OutProfile.Emitters[Index].CameraResponse = ToColor(Response);
        EmitterSum += Response;
        OutReport += FString::Printf(TEXT("エミッタ %s: (%.4f, %.4f, %.4f)\n"), *Base.Emitters[Index].Name.ToString(), Response.X, Response.Y, Response.Z);
    }

    // --- 調光カーブ (全エミッタを同じレベルで点けた輝度の比) ---
    const FLightSyncCalibrationStep *Full = FindStep(Steps, [](const FLightSyncCalibrationStep &S)
                                                     { return S.Kind == ELightSyncCalibrationStepKind::FixtureAllLevel && FMath::IsNearlyEqual(S.Level, 1.0f); });
    if (Full)
    {
        const FVector3d FullResponse = LinearizeCamera(Transfer, Full->CameraCode) - Black;
        const double FullLum = Luminance(FullResponse);

        // 加法性の確認: 各エミッタ単独の和 ≈ 全点灯 (ずれる灯体はエミッタ間の干渉や電源の制限がある)
        if (Base.ColorModel == ELightSyncFixtureColorModel::Emitters && FullLum > 0.0)
        {
            const double Additivity = (EmitterSum - FullResponse).Size() / FMath::Max(FullResponse.Size(), 1e-9);
            OutReport += FString::Printf(TEXT("加法性の誤差 (単独の和 vs 全点灯): %.2f%%%s\n"), Additivity * 100.0,
                                         Additivity > 0.05 ? TEXT("  ※5%超: 全点灯時に出力が落ちる灯体の可能性") : TEXT(""));
        }

        TArray<double> Levels, Measured;
        TArray<FVector2D> LUT;
        LUT.Add(FVector2D(0.0, 0.0));
        for (const FLightSyncCalibrationStep &Step : Steps)
        {
            if (Step.bMeasured && Step.Kind == ELightSyncCalibrationStepKind::FixtureAllLevel && FullLum > 0.0)
            {
                const double Y = Luminance(LinearizeCamera(Transfer, Step.CameraCode) - Black) / FullLum;
                Levels.Add(Step.Level);
                Measured.Add(Y);
                LUT.Add(FVector2D(Step.Level, Y));
            }
        }

        if (Levels.Num() >= 2)
        {
            const double Gamma = FitGammaFromSteps(Levels, Measured);
            double MaxError = 0.0;
            for (int32 Index = 0; Index < Levels.Num(); ++Index)
            {
                MaxError = FMath::Max(MaxError, FMath::Abs(FMath::Pow(Levels[Index], Gamma) - Measured[Index]));
            }

            LUT.Sort([](const FVector2D &A, const FVector2D &B) { return A.X < B.X; });
            OutProfile.DimmingLUT = LUT;
            if (Gamma > 0.0 && MaxError < 0.02)
            {
                OutProfile.DimmingCurve = FMath::IsNearlyEqual(Gamma, 1.0, 0.02) ? ELightSyncDimmingCurve::Linear : ELightSyncDimmingCurve::Gamma;
                OutProfile.DimmingGamma = static_cast<float>(Gamma);
                OutReport += FString::Printf(TEXT("調光カーブ: ガンマ %.3f (最大誤差 %.2f%%)\n"), Gamma, MaxError * 100.0);
            }
            else
            {
                OutProfile.DimmingCurve = ELightSyncDimmingCurve::LUT;
                OutReport += FString::Printf(TEXT("調光カーブ: 測定点の LUT (ガンマ近似の誤差 %.2f%% が大きいため)\n"), MaxError * 100.0);
            }
        }
    }
    else
    {
        OutReport += TEXT("調光カーブ: 100% の測定がないので変更しません\n");
    }

    return true;
}

bool ULightSyncCalibrator::FitStageFromSteps(const TArray<FLightSyncCalibrationStep> &Steps, ELightSyncCameraTransfer Transfer,
                                            FLinearColor &OutRed, FLinearColor &OutGreen, FLinearColor &OutBlue, FString &OutReport)
{
    OutReport.Reset();

    const FLightSyncCalibrationStep *Black = FindStep(Steps, [](const FLightSyncCalibrationStep &S)
                                                      { return S.Kind == ELightSyncCalibrationStepKind::WallPatch && S.PatchColor.IsAlmostBlack(); });
    if (!Black)
    {
        OutReport = TEXT("「壁 黒」の測定がありません");
        return false;
    }
    const FVector3d CameraBlack = LinearizeCamera(Transfer, Black->CameraCode);
    const FVector3d ReferenceBlack = ToVec(Black->ProbeReference);

    TArray<FVector3d> Reference, Measured;
    for (const FLightSyncCalibrationStep &Step : Steps)
    {
        if (Step.bMeasured && Step.Kind == ELightSyncCalibrationStepKind::WallPatch && !Step.PatchColor.IsAlmostBlack())
        {
            Reference.Add(ToVec(Step.ProbeReference) - ReferenceBlack);
            Measured.Add(LinearizeCamera(Transfer, Step.CameraCode) - CameraBlack);
        }
    }

    if (Reference.Num() < 3)
    {
        OutReport = TEXT("色パッチの測定が 3 つ以上必要です (赤・緑・青)");
        return false;
    }

    FMatrix3x3 Matrix;
    double Rms = 0.0;
    if (!Fit3x3FromPatches(Reference, Measured, Matrix, &Rms))
    {
        OutReport = TEXT("行列を求められませんでした (パッチの色が似すぎているか、プローブの基準値が 0)");
        return false;
    }

    OutRed = ToColor(Matrix.GetColumn(0));
    OutGreen = ToColor(Matrix.GetColumn(1));
    OutBlue = ToColor(Matrix.GetColumn(2));

    double Scale = 0.0;
    for (const FVector3d &M : Measured)
    {
        Scale = FMath::Max(Scale, M.Size());
    }
    OutReport = FString::Printf(TEXT("M_ref 列: R (%.4f, %.4f, %.4f)  G (%.4f, %.4f, %.4f)  B (%.4f, %.4f, %.4f)\n残差 RMS: %.2f%% (最大測定値比)\n"),
                                OutRed.R, OutRed.G, OutRed.B, OutGreen.R, OutGreen.G, OutGreen.B, OutBlue.R, OutBlue.G, OutBlue.B,
                                Scale > 0.0 ? Rms / Scale * 100.0 : 0.0);
    return true;
}

bool ULightSyncCalibrator::FitFixture(ULightSyncFixtureProfile *Target, FString &OutReport)
{
    if (!Target && Probe && Probe->DMXOutput && Probe->DMXOutput->FixtureMappings.IsValidIndex(FixtureIndex))
    {
        Target = Probe->DMXOutput->FixtureMappings[FixtureIndex].Profile;
    }
    if (!Target)
    {
        OutReport = TEXT("書き込み先のプロファイルアセットがありません (先にプロファイルを作成してください)");
        LastReport = OutReport;
        return false;
    }

    FLightSyncFixtureProfileData Fitted;
    if (!FitFixtureFromSteps(Target->Profile, FixtureSteps, CameraTransfer, Fitted, OutReport))
    {
        LastReport = OutReport;
        return false;
    }

    Target->Modify();
    Target->Profile = Fitted;
    Target->MarkPackageDirty();
    LastReport = OutReport;
    return true;
}

bool ULightSyncCalibrator::FitStage(ULightSyncStageCalibration *Target, FString &OutReport)
{
    if (!Target && Probe && Probe->DMXOutput)
    {
        Target = Probe->DMXOutput->StageCalibration;
    }
    if (!Target)
    {
        OutReport = TEXT("書き込み先のステージキャリブレーションがありません (先に作成してください)");
        LastReport = OutReport;
        return false;
    }

    FLinearColor Red, Green, Blue;
    if (!FitStageFromSteps(WallSteps, CameraTransfer, Red, Green, Blue, OutReport))
    {
        LastReport = OutReport;
        return false;
    }

    Target->Modify();
    Target->ReferenceRed = Red;
    Target->ReferenceGreen = Green;
    Target->ReferenceBlue = Blue;
    Target->MarkPackageDirty();
    LastReport = OutReport;
    return true;
}

// ============================================================
// アセット作成 (エディタ)
// ============================================================

#if WITH_EDITOR
namespace
{
    template <typename AssetType>
    AssetType *CreateAsset(const FString &Folder, const FString &BaseName)
    {
        FString SafeName = BaseName;
        for (TCHAR &Ch : SafeName)
        {
            if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
            {
                Ch = TEXT('_');
            }
        }

        FString PackageName = Folder / SafeName;
        for (int32 Suffix = 1; FPackageName::DoesPackageExist(PackageName) || FindPackage(nullptr, *PackageName); ++Suffix)
        {
            PackageName = Folder / FString::Printf(TEXT("%s_%d"), *SafeName, Suffix);
        }

        UPackage *Package = CreatePackage(*PackageName);
        AssetType *Asset = NewObject<AssetType>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone | RF_Transactional);
        FAssetRegistryModule::AssetCreated(Asset);
        Package->MarkPackageDirty();
        return Asset;
    }
}

ULightSyncFixtureProfile *ULightSyncCalibrator::CreateProfileAssetForFixture()
{
    if (!Probe || !Probe->DMXOutput || !Probe->DMXOutput->FixtureMappings.IsValidIndex(FixtureIndex))
    {
        return nullptr;
    }

    FDMXFixtureMapping &Mapping = Probe->DMXOutput->FixtureMappings[FixtureIndex];
    if (Mapping.Profile)
    {
        return Mapping.Profile;
    }

    ULightSyncFixtureProfile *Asset = CreateAsset<ULightSyncFixtureProfile>(TEXT("/Game/LightSync/Profiles"), TEXT("FP_") + Mapping.FixtureName);
    Asset->FixtureModel = Mapping.FixtureName;
    Asset->Profile = LightSyncFixture::MakePreset(Mapping.ColorMode);

    Probe->Modify();
    Probe->DMXOutput->Modify();
    Mapping.Profile = Asset;
    Probe->MarkPackageDirty();
    return Asset;
}

ULightSyncStageCalibration *ULightSyncCalibrator::CreateStageCalibrationAsset()
{
    if (!Probe || !Probe->DMXOutput)
    {
        return nullptr;
    }
    if (Probe->DMXOutput->StageCalibration)
    {
        return Probe->DMXOutput->StageCalibration;
    }

    ULightSyncStageCalibration *Asset = CreateAsset<ULightSyncStageCalibration>(TEXT("/Game/LightSync"), TEXT("StageCalibration"));

    Probe->Modify();
    Probe->DMXOutput->Modify();
    Probe->DMXOutput->StageCalibration = Asset;
    if (Probe->OSCOutput)
    {
        Probe->OSCOutput->Modify();
        Probe->OSCOutput->StageCalibration = Asset;
    }
    Probe->MarkPackageDirty();
    return Asset;
}
#endif
