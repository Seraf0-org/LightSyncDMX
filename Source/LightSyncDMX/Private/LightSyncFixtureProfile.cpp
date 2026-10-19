// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncFixtureProfile.h"

using namespace LightSyncColor;

// ============================================================
// FLightSyncFixtureProfileData
// ============================================================

int32 FLightSyncFixtureProfileData::GetChannelCount() const
{
    int32 Count = 0;
    for (const FLightSyncFixtureChannel &Channel : Channels)
    {
        Count += Channel.GetWidth();
    }
    return Count;
}

bool FLightSyncFixtureProfileData::HasDimmer() const
{
    return Channels.ContainsByPredicate([](const FLightSyncFixtureChannel &Channel)
                                        { return Channel.Function == ELightSyncChannelFunction::Dimmer; });
}

FDimmingCurve FLightSyncFixtureProfileData::MakeDimmingCurve() const
{
    FDimmingCurve Curve;
    switch (DimmingCurve)
    {
    case ELightSyncDimmingCurve::Gamma:
        Curve.Type = FDimmingCurve::EType::Gamma;
        Curve.Gamma = FMath::Max(DimmingGamma, 0.1f);
        break;
    case ELightSyncDimmingCurve::LUT:
        Curve.Type = FDimmingCurve::EType::LUT;
        for (const FVector2D &Point : DimmingLUT)
        {
            Curve.Points.Add(FVector2d(Point.X, Point.Y));
        }
        break;
    default:
        Curve.Type = FDimmingCurve::EType::Linear;
        break;
    }
    return Curve;
}

// ============================================================
// アセット
// ============================================================

void ULightSyncFixtureProfile::ResetToRGBPreset()
{
    InitializeFromPreset(ELightSyncColorMode::RGB);
}

void ULightSyncFixtureProfile::InitializeFromPreset(ELightSyncColorMode Mode)
{
    Modify();
    Profile = LightSyncFixture::MakePreset(Mode);
}

FMatrix3x3 ULightSyncStageCalibration::GetReferenceToCamera() const
{
    auto ToVec = [](const FLinearColor &C) { return FVector3d(C.R, C.G, C.B); };
    return FMatrix3x3::FromColumns(ToVec(ReferenceRed), ToVec(ReferenceGreen), ToVec(ReferenceBlue));
}

// ============================================================
// プリセットと計算
// ============================================================

namespace LightSyncFixture
{
    static FLightSyncEmitter MakeEmitter(FName Name, const FLinearColor &Response, bool bIsWhite = false)
    {
        FLightSyncEmitter Emitter;
        Emitter.Name = Name;
        Emitter.CameraResponse = Response;
        Emitter.bIsWhite = bIsWhite;
        return Emitter;
    }

    static FLightSyncFixtureChannel MakeChannel(FName Label, ELightSyncChannelFunction Function, int32 EmitterIndex = 0, int32 FixedValue = 0)
    {
        FLightSyncFixtureChannel Channel;
        Channel.Label = Label;
        Channel.Function = Function;
        Channel.EmitterIndex = EmitterIndex;
        Channel.FixedValue = FixedValue;
        return Channel;
    }

    FLightSyncFixtureProfileData MakePreset(ELightSyncColorMode Mode)
    {
        // 未計測の理想応答: カメラ = 作業色空間 (Rec.709) とみなす
        const FLinearColor Red(1.0f, 0.0f, 0.0f), Green(0.0f, 1.0f, 0.0f), Blue(0.0f, 0.0f, 1.0f), White(1.0f, 1.0f, 1.0f);
        // 590nm 前後のアンバー LED を Rec.709 に射影した近似 (Rec.709 の外側にあるので B は 0 で切っている)。要キャリブレーション
        const FLinearColor Amber(1.0f, 0.2f, 0.0f);

        using F = ELightSyncChannelFunction;
        FLightSyncFixtureProfileData P;

        auto AddRGB = [&]()
        {
            P.Emitters.Add(MakeEmitter(TEXT("R"), Red));
            P.Emitters.Add(MakeEmitter(TEXT("G"), Green));
            P.Emitters.Add(MakeEmitter(TEXT("B"), Blue));
        };

        switch (Mode)
        {
        case ELightSyncColorMode::RGB:
            AddRGB();
            P.Channels = {MakeChannel(TEXT("R"), F::Emitter, 0), MakeChannel(TEXT("G"), F::Emitter, 1), MakeChannel(TEXT("B"), F::Emitter, 2)};
            break;

        case ELightSyncColorMode::RGBW:
            AddRGB();
            P.Emitters.Add(MakeEmitter(TEXT("W"), White, true));
            P.Channels = {MakeChannel(TEXT("R"), F::Emitter, 0), MakeChannel(TEXT("G"), F::Emitter, 1),
                          MakeChannel(TEXT("B"), F::Emitter, 2), MakeChannel(TEXT("W"), F::Emitter, 3)};
            break;

        case ELightSyncColorMode::RGBAW:
            AddRGB();
            P.Emitters.Add(MakeEmitter(TEXT("A"), Amber));
            P.Emitters.Add(MakeEmitter(TEXT("W"), White, true));
            P.Channels = {MakeChannel(TEXT("R"), F::Emitter, 0), MakeChannel(TEXT("G"), F::Emitter, 1), MakeChannel(TEXT("B"), F::Emitter, 2),
                          MakeChannel(TEXT("A"), F::Emitter, 3), MakeChannel(TEXT("W"), F::Emitter, 4)};
            break;

        case ELightSyncColorMode::DRGB:
            AddRGB();
            P.Channels = {MakeChannel(TEXT("Dimmer"), F::Dimmer), MakeChannel(TEXT("R"), F::Emitter, 0),
                          MakeChannel(TEXT("G"), F::Emitter, 1), MakeChannel(TEXT("B"), F::Emitter, 2)};
            break;

        case ELightSyncColorMode::CCT:
            P.ColorModel = ELightSyncFixtureColorModel::CCT;
            P.Emitters.Add(MakeEmitter(TEXT("White"), White, true));
            P.Channels = {MakeChannel(TEXT("CCT"), F::CCT), MakeChannel(TEXT("Dimmer"), F::Dimmer)};
            break;

        case ELightSyncColorMode::CCTRGBW10CH:
            // RGBW モード (Crossfade = 255) で使う。CCT / G-M は無効値を固定で送る
            AddRGB();
            P.Emitters.Add(MakeEmitter(TEXT("W"), White, true));
            P.CCTMaxKelvin = 7500.0f;
            P.Channels = {MakeChannel(TEXT("CCT"), F::CCT, 0, 0),
                          MakeChannel(TEXT("GreenMagenta"), F::Fixed, 0, 0),
                          MakeChannel(TEXT("Dimmer"), F::Dimmer),
                          MakeChannel(TEXT("Crossfade"), F::Fixed, 0, 255),
                          MakeChannel(TEXT("R"), F::Emitter, 0), MakeChannel(TEXT("G"), F::Emitter, 1),
                          MakeChannel(TEXT("B"), F::Emitter, 2), MakeChannel(TEXT("W"), F::Emitter, 3),
                          MakeChannel(TEXT("Strobe"), F::Fixed, 0, 0),
                          MakeChannel(TEXT("Reserved"), F::Fixed, 0, 0)};
            break;

        case ELightSyncColorMode::NanlitePavoSlimRGBW11CH:
            // Nanlite PavoSlim DMX REFERENCE GUIDE「CCT & RGBW 8bit」。RGBW のみで使う (Crossfade 255)
            AddRGB();
            P.Emitters.Add(MakeEmitter(TEXT("W"), White, true));
            P.CCTMinKelvin = 2700.0f;
            P.CCTMaxKelvin = 7500.0f;
            P.Channels = {MakeChannel(TEXT("Dimmer"), F::Dimmer),
                          MakeChannel(TEXT("CCT"), F::Fixed, 0, 0),
                          MakeChannel(TEXT("GreenMagenta"), F::Fixed, 0, 132),   // 120-145 = ニュートラル
                          MakeChannel(TEXT("Crossfade"), F::Fixed, 0, 255),      // 255 = RGBW のみ
                          MakeChannel(TEXT("R"), F::Emitter, 0), MakeChannel(TEXT("G"), F::Emitter, 1),
                          MakeChannel(TEXT("B"), F::Emitter, 2), MakeChannel(TEXT("W"), F::Emitter, 3),
                          MakeChannel(TEXT("Strobe"), F::Fixed, 0, 0),           // 0-3 = OFF
                          MakeChannel(TEXT("Reserved"), F::Fixed, 0, 0),
                          MakeChannel(TEXT("Reserved"), F::Fixed, 0, 0)};
            break;
        }
        return P;
    }

    const FLightSyncFixtureProfileData& GetPreset(ELightSyncColorMode Mode)
    {
        static const TArray<FLightSyncFixtureProfileData> Presets = []()
        {
            TArray<FLightSyncFixtureProfileData> All;
            for (int32 Index = 0; Index <= static_cast<int32>(ELightSyncColorMode::NanlitePavoSlimRGBW11CH); ++Index)
            {
                All.Add(MakePreset(static_cast<ELightSyncColorMode>(Index)));
            }
            return All;
        }();
        return Presets[FMath::Clamp(static_cast<int32>(Mode), 0, Presets.Num() - 1)];
    }

    static void WriteLevel(TArray<uint8> &Out, const FLightSyncFixtureChannel &Channel, double Level)
    {
        uint8 Coarse = 0, Fine = 0;
        QuantizeToDMX(FMath::Clamp(Level, 0.0, 1.0), Channel.b16Bit, Coarse, Fine);
        Out.Add(Coarse);
        if (Channel.b16Bit)
        {
            Out.Add(Fine);
        }
    }

    static void WriteFixed(TArray<uint8> &Out, const FLightSyncFixtureChannel &Channel)
    {
        if (Channel.b16Bit)
        {
            const int32 Value = FMath::Clamp(Channel.FixedValue, 0, 65535);
            Out.Add(static_cast<uint8>(Value >> 8));
            Out.Add(static_cast<uint8>(Value & 0xFF));
        }
        else
        {
            Out.Add(static_cast<uint8>(FMath::Clamp(Channel.FixedValue, 0, 255)));
        }
    }

    static double CameraLuminance(const FVector3d &RGB)
    {
        // FEmitterSet の既定と同じ重み (カメラ RGB を Rec.709 相当とみなす)
        return 0.2126 * RGB.X + 0.7152 * RGB.Y + 0.0722 * RGB.Z;
    }

    static FVector3d ToVec(const FLinearColor &C)
    {
        return FVector3d(C.R, C.G, C.B);
    }

    FLightSyncFixtureOutput Compute(const FLightSyncFixtureProfileData &Profile, const FLinearColor &TargetWorking,
                                    const FMatrix3x3 &ReferenceToCamera, double Gain)
    {
        FLightSyncFixtureOutput Out;
        const FDimmingCurve Curve = Profile.MakeDimmingCurve();
        const bool bHasDimmer = Profile.HasDimmer();

        // 目標 (負値は物理的にありえないので 0) → カメラ空間
        const double SafeGain = FMath::Max(Gain, 0.0);
        const FVector3d Target(FMath::Max<double>(TargetWorking.R, 0.0) * SafeGain,
                               FMath::Max<double>(TargetWorking.G, 0.0) * SafeGain,
                               FMath::Max<double>(TargetWorking.B, 0.0) * SafeGain);
        const FVector3d TargetCamera = ReferenceToCamera.Transform(Target);

        if (Profile.ColorModel == ELightSyncFixtureColorModel::Emitters)
        {
            FEmitterSet Set;
            for (int32 Index = 0; Index < Profile.Emitters.Num(); ++Index)
            {
                const FLightSyncEmitter &Emitter = Profile.Emitters[Index];
                Set.Responses.Add(ToVec(Emitter.CameraResponse));
                if (Emitter.bIsWhite && Set.PreferredWhiteIndex == INDEX_NONE)
                {
                    Set.PreferredWhiteIndex = Index;
                }
            }
            Set.WhitePreference = FMath::Clamp<double>(Profile.WhitePreference, 0.0, 1.0);

            if (Set.Responses.Num() > 0)
            {
                const FSolveResult Solve = SolveEmitterLevels(Set, TargetCamera);
                Out.EmitterOutputs = Solve.Levels;
                Out.bOutOfGamut = Solve.bOutOfGamut;
                Out.bClipped = Solve.bClipped;
            }

            double MaxOutput = 0.0;
            for (double Level : Out.EmitterOutputs)
            {
                MaxOutput = FMath::Max(MaxOutput, Level);
            }
            Out.Intensity = MaxOutput;

            for (const FLightSyncFixtureChannel &Channel : Profile.Channels)
            {
                switch (Channel.Function)
                {
                case ELightSyncChannelFunction::Emitter:
                {
                    const double Output = Out.EmitterOutputs.IsValidIndex(Channel.EmitterIndex) ? Out.EmitterOutputs[Channel.EmitterIndex] : 0.0;
                    // Dimmer があればエミッタは混色比 (リニア)、なければエミッタごとに調光カーブの逆
                    const double Level = bHasDimmer ? (MaxOutput > 0.0 ? Output / MaxOutput : 0.0) : Curve.Invert(Output);
                    WriteLevel(Out.Values, Channel, Level);
                    break;
                }
                case ELightSyncChannelFunction::Dimmer:
                    WriteLevel(Out.Values, Channel, Curve.Invert(MaxOutput));
                    break;
                default:
                    WriteFixed(Out.Values, Channel);
                    break;
                }
            }
            return Out;
        }

        // ---- CCT モデル ----
        const FVector3d WhiteResponse = Profile.Emitters.Num() > 0 ? ToVec(Profile.Emitters[0].CameraResponse) : FVector3d(1.0, 1.0, 1.0);
        double Intensity = CameraLuminance(TargetCamera) / FMath::Max(CameraLuminance(WhiteResponse), 1e-9);

        double CCT = 0.5 * (Profile.CCTMinKelvin + Profile.CCTMaxKelvin);
        double Duv = 0.0;
        if (!XYZToCCTDuv(WorkingRGBToXYZ(Target), CCT, Duv))
        {
            Intensity = 0.0;
            CCT = 0.5 * (Profile.CCTMinKelvin + Profile.CCTMaxKelvin);
            Duv = 0.0;
        }

        if (Intensity > 1.0)
        {
            Intensity = 1.0;
            Out.bClipped = true;
        }
        Intensity = FMath::Max(Intensity, 0.0);

        const double CCTSpan = FMath::Max<double>(Profile.CCTMaxKelvin - Profile.CCTMinKelvin, 1.0);
        double CCTNorm = (CCT - Profile.CCTMinKelvin) / CCTSpan;
        const double TintSpan = FMath::Max<double>(Profile.TintMaxDuv - Profile.TintMinDuv, 1e-6);
        double TintNorm = (Duv - Profile.TintMinDuv) / TintSpan;

        const bool bHasTintChannel = Profile.Channels.ContainsByPredicate([](const FLightSyncFixtureChannel &C) { return C.Function == ELightSyncChannelFunction::Tint; });
        if (Intensity > 0.0 && (CCTNorm < 0.0 || CCTNorm > 1.0 || (bHasTintChannel && (TintNorm < 0.0 || TintNorm > 1.0))))
        {
            Out.bOutOfGamut = true;
        }
        CCTNorm = FMath::Clamp(CCTNorm, 0.0, 1.0);
        TintNorm = FMath::Clamp(TintNorm, 0.0, 1.0);

        Out.CCT = CCT;
        Out.Duv = Duv;
        Out.Intensity = Intensity;

        for (const FLightSyncFixtureChannel &Channel : Profile.Channels)
        {
            switch (Channel.Function)
            {
            case ELightSyncChannelFunction::CCT:
                WriteLevel(Out.Values, Channel, CCTNorm);
                break;
            case ELightSyncChannelFunction::Tint:
                WriteLevel(Out.Values, Channel, TintNorm);
                break;
            case ELightSyncChannelFunction::Dimmer:
            case ELightSyncChannelFunction::Emitter:
                WriteLevel(Out.Values, Channel, Curve.Invert(Intensity));
                break;
            default:
                WriteFixed(Out.Values, Channel);
                break;
            }
        }
        return Out;
    }

    FLightSyncFixtureOutput ComputeBlackout(const FLightSyncFixtureProfileData &Profile)
    {
        FLightSyncFixtureOutput Out;
        const double MidCCT = 0.5;
        for (const FLightSyncFixtureChannel &Channel : Profile.Channels)
        {
            switch (Channel.Function)
            {
            case ELightSyncChannelFunction::Emitter:
            case ELightSyncChannelFunction::Dimmer:
                WriteLevel(Out.Values, Channel, 0.0);
                break;
            case ELightSyncChannelFunction::CCT:
                if (Profile.ColorModel == ELightSyncFixtureColorModel::CCT)
                {
                    WriteLevel(Out.Values, Channel, MidCCT);
                }
                else
                {
                    WriteFixed(Out.Values, Channel);
                }
                break;
            case ELightSyncChannelFunction::Tint:
                if (Profile.ColorModel == ELightSyncFixtureColorModel::CCT)
                {
                    const double Neutral = (0.0 - Profile.TintMinDuv) / FMath::Max<double>(Profile.TintMaxDuv - Profile.TintMinDuv, 1e-6);
                    WriteLevel(Out.Values, Channel, Neutral);
                }
                else
                {
                    WriteFixed(Out.Values, Channel);
                }
                break;
            default:
                WriteFixed(Out.Values, Channel);
                break;
            }
        }
        return Out;
    }

    FLightSyncFixtureOutput ComputeTestPattern(const FLightSyncFixtureProfileData &Profile,
                                               const TArray<double> &EmitterLevels, double DimmerLevel)
    {
        FLightSyncFixtureOutput Out;
        const bool bCCTModel = Profile.ColorModel == ELightSyncFixtureColorModel::CCT;
        for (const FLightSyncFixtureChannel &Channel : Profile.Channels)
        {
            switch (Channel.Function)
            {
            case ELightSyncChannelFunction::Emitter:
                if (bCCTModel)
                {
                    WriteLevel(Out.Values, Channel, DimmerLevel);
                }
                else
                {
                    WriteLevel(Out.Values, Channel, EmitterLevels.IsValidIndex(Channel.EmitterIndex) ? EmitterLevels[Channel.EmitterIndex] : 0.0);
                }
                break;
            case ELightSyncChannelFunction::Dimmer:
                WriteLevel(Out.Values, Channel, DimmerLevel);
                break;
            case ELightSyncChannelFunction::CCT:
                if (bCCTModel)
                {
                    WriteLevel(Out.Values, Channel, 0.5);
                }
                else
                {
                    WriteFixed(Out.Values, Channel);
                }
                break;
            case ELightSyncChannelFunction::Tint:
                if (bCCTModel)
                {
                    const double Neutral = (0.0 - Profile.TintMinDuv) / FMath::Max<double>(Profile.TintMaxDuv - Profile.TintMinDuv, 1e-6);
                    WriteLevel(Out.Values, Channel, Neutral);
                }
                else
                {
                    WriteFixed(Out.Values, Channel);
                }
                break;
            default:
                WriteFixed(Out.Values, Channel);
                break;
            }
        }
        return Out;
    }
}

