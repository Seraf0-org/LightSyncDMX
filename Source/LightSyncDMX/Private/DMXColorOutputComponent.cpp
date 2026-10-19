// Copyright UE-Comp. All Rights Reserved.

#include "DMXColorOutputComponent.h"
#include "LightSyncDMXModule.h"

#include "IO/DMXOutputPort.h"
#include "IO/DMXPortManager.h"
#include "DMXProtocolTypes.h"
#include "DMXProtocolConstants.h"
#include "Library/DMXLibrary.h"

UDMXColorOutputComponent::UDMXColorOutputComponent()
{
    PrimaryComponentTick.bCanEverTick = false;

    // デフォルトで1つのフィクスチャマッピングを追加
    FDMXFixtureMapping DefaultMapping;
    DefaultMapping.FixtureName = TEXT("LED_01");
    DefaultMapping.Universe = 1;
    DefaultMapping.StartChannel = 1;
    DefaultMapping.ColorMode = ELightSyncColorMode::RGB;
    FixtureMappings.Add(DefaultMapping);
}

void UDMXColorOutputComponent::BeginPlay()
{
    Super::BeginPlay();

    UE_LOG(LogLightSyncDMX, Log, TEXT("DMXColorOutput: %d fixture mapping(s) configured."),
           FixtureMappings.Num());
}

void UDMXColorOutputComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // シャットダウン時にブラックアウトを送信
    Blackout();

    Super::EndPlay(EndPlayReason);
}

void UDMXColorOutputComponent::SendColor(const FLinearColor &Color)
{
    if (!bDMXOutputEnabled)
    {
        return;
    }

    LastSentColor = Color;

    for (const FDMXFixtureMapping &Mapping : FixtureMappings)
    {
        if (Mapping.bEnabled)
        {
            SendDMXData(Mapping, Color);
        }
    }

    SentFrameCount++;
}

void UDMXColorOutputComponent::SendZoneColors(const TArray<FLightSyncZoneResult> &ZoneResults)
{
    if (!bDMXOutputEnabled)
    {
        return;
    }

    for (const FDMXFixtureMapping &Mapping : FixtureMappings)
    {
        if (!Mapping.bEnabled)
        {
            continue;
        }

        const FLightSyncZoneResult *Found = ZoneResults.FindByPredicate(
            [&Mapping](const FLightSyncZoneResult &Result) { return Result.Name == Mapping.Zone; });

        if (!Found)
        {
            // 結果が出る前 (起動直後) は黙ってスキップ。結果があるのに見つからないときだけ警告
            if (ZoneResults.Num() > 0 && !WarnedMissingZoneFixtures.Contains(Mapping.FixtureName))
            {
                UE_LOG(LogLightSyncDMX, Warning, TEXT("DMXColorOutput: フィクスチャ '%s' のゾーン '%s' がプローブにありません。"),
                       *Mapping.FixtureName, *Mapping.Zone.ToString());
                WarnedMissingZoneFixtures.Add(Mapping.FixtureName);
            }
            continue;
        }
        WarnedMissingZoneFixtures.Remove(Mapping.FixtureName);

        SendDMXData(Mapping, Found->Color);
        LastSentColor = Found->Color;
    }

    SentFrameCount++;
}

void UDMXColorOutputComponent::SendColorToFixture(const FLinearColor &Color, int32 FixtureIndex)
{
    if (!bDMXOutputEnabled || !FixtureMappings.IsValidIndex(FixtureIndex))
    {
        return;
    }

    const FDMXFixtureMapping &Mapping = FixtureMappings[FixtureIndex];
    if (Mapping.bEnabled)
    {
        SendDMXData(Mapping, Color);
    }
}

void UDMXColorOutputComponent::Blackout()
{
    for (const FDMXFixtureMapping &Mapping : FixtureMappings)
    {
        SendDMXData(Mapping, FLinearColor::Black);
    }
}

void UDMXColorOutputComponent::AddFixtureMapping(const FDMXFixtureMapping &Mapping)
{
    FixtureMappings.Add(Mapping);
}

void UDMXColorOutputComponent::RemoveFixtureMapping(int32 Index)
{
    if (FixtureMappings.IsValidIndex(Index))
    {
        FixtureMappings.RemoveAt(Index);
    }
}

int32 UDMXColorOutputComponent::GetChannelCount(ELightSyncColorMode Mode)
{
    switch (Mode)
    {
    case ELightSyncColorMode::RGB:         return 3;
    case ELightSyncColorMode::RGBW:        return 4;
    case ELightSyncColorMode::RGBAW:       return 5;
    case ELightSyncColorMode::DRGB:        return 4;
    case ELightSyncColorMode::CCT:         return 2;
    case ELightSyncColorMode::CCTRGBW10CH: return 10;
    }
    return 0;
}

bool UDMXColorOutputComponent::IsMappingInRange(const FDMXFixtureMapping &Mapping)
{
    const int32 LastChannel = Mapping.StartChannel + GetChannelCount(Mapping.ColorMode) - 1;
    return Mapping.StartChannel >= 1 && LastChannel <= DMX_UNIVERSE_SIZE;
}

void UDMXColorOutputComponent::GatherOutputPorts(TArray<FDMXOutputPortSharedRef> &OutPorts)
{
    OutPorts.Reset();

    // DMXLibrary 指定時はライブラリの出力ポートのみを使う
    if (!DMXLibrary.IsNull())
    {
        if (!LoadedDMXLibrary || LoadedDMXLibrary != DMXLibrary.Get())
        {
            LoadedDMXLibrary = DMXLibrary.LoadSynchronous();
        }

        if (LoadedDMXLibrary)
        {
            OutPorts = LoadedDMXLibrary->GetOutputPorts().Array();
            if (OutPorts.Num() > 0)
            {
                bWarnedLibraryNoPorts = false;
                return;
            }

            if (!bWarnedLibraryNoPorts)
            {
                UE_LOG(LogLightSyncDMX, Warning,
                       TEXT("DMXColorOutput: DMXライブラリ '%s' に出力ポートがありません。全出力ポートに送信します。"),
                       *LoadedDMXLibrary->GetName());
                bWarnedLibraryNoPorts = true;
            }
        }
    }

    OutPorts = FDMXPortManager::Get().GetOutputPorts();
}

void UDMXColorOutputComponent::SendDMXData(const FDMXFixtureMapping &Mapping, const FLinearColor &Color)
{
    // ユニバース (512ch) をはみ出すマッピングは送信しない (隣のユニバースや無関係なchを汚さないため)
    if (!IsMappingInRange(Mapping))
    {
        if (!WarnedOutOfRangeFixtures.Contains(Mapping.FixtureName))
        {
            UE_LOG(LogLightSyncDMX, Warning,
                   TEXT("DMXColorOutput: フィクスチャ '%s' は Ch%d から %dch 使用するため 512ch を超えます。送信をスキップします。"),
                   *Mapping.FixtureName, Mapping.StartChannel, GetChannelCount(Mapping.ColorMode));
            WarnedOutOfRangeFixtures.Add(Mapping.FixtureName);
        }
        return;
    }
    WarnedOutOfRangeFixtures.Remove(Mapping.FixtureName);

    // マッピングに基づいてチャンネルデータを構築
    TMap<int32, uint8> ChannelData = ColorToDMXChannels(Mapping, Color);

    if (ChannelData.Num() == 0)
    {
        return;
    }

    TArray<FDMXOutputPortSharedRef> OutputPorts;
    GatherOutputPorts(OutputPorts);

    if (OutputPorts.Num() == 0)
    {
        if (!bWarnedNoOutputPorts)
        {
            UE_LOG(LogLightSyncDMX, Warning,
                   TEXT("DMXColorOutput: DMX出力ポートが設定されていません。"
                        "Project Settings → Plugins → DMX Protocol で出力ポートを設定してください。"));
            bWarnedNoOutputPorts = true;
        }
        return;
    }

    bWarnedNoOutputPorts = false;

    for (const FDMXOutputPortSharedRef &OutputPort : OutputPorts)
    {
        OutputPort->SendDMX(Mapping.Universe, ChannelData);
    }

    UE_LOG(LogLightSyncDMX, VeryVerbose,
           TEXT("DMX Sent: Fixture='%s' Univ=%d Ch=%d Color=(%.2f, %.2f, %.2f)"),
           *Mapping.FixtureName, Mapping.Universe, Mapping.StartChannel,
           Color.R, Color.G, Color.B);
}

TMap<int32, uint8> UDMXColorOutputComponent::ColorToDMXChannels(
    const FDMXFixtureMapping &Mapping,
    const FLinearColor &Color) const
{
    TMap<int32, uint8> Channels;

    // === ヘルパー: float 0-1 → uint8 0-255 (四捨五入) ===
    auto FloatToByte = [](float Value) -> uint8
    {
        return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Value * 255.0f), 0, 255));
    };

    // 明度スケーリング適用。上限を超えたら全チャンネルを同じ比率で下げて色相を保つ
    const float Gain = Mapping.BrightnessScale * MasterDimmer;
    float R = FMath::Max(Color.R * Gain, 0.0f);
    float G = FMath::Max(Color.G * Gain, 0.0f);
    float B = FMath::Max(Color.B * Gain, 0.0f);
    const float PeakComp = FMath::Max3(R, G, B);
    if (PeakComp > 1.0f)
    {
        R /= PeakComp;
        G /= PeakComp;
        B /= PeakComp;
    }

    const int32 Ch = Mapping.StartChannel;

    switch (Mapping.ColorMode)
    {
    case ELightSyncColorMode::RGB:
        // 3ch: R, G, B
        Channels.Add(Ch + 0, FloatToByte(R));
        Channels.Add(Ch + 1, FloatToByte(G));
        Channels.Add(Ch + 2, FloatToByte(B));
        break;

    case ELightSyncColorMode::RGBW:
    {
        // 4ch: R, G, B, W
        float OutR, OutG, OutB, OutW;
        RGBtoRGBW(R, G, B, OutR, OutG, OutB, OutW);
        Channels.Add(Ch + 0, FloatToByte(OutR));
        Channels.Add(Ch + 1, FloatToByte(OutG));
        Channels.Add(Ch + 2, FloatToByte(OutB));
        Channels.Add(Ch + 3, FloatToByte(OutW));
        break;
    }

    case ELightSyncColorMode::RGBAW:
    {
        // 5ch: R, G, B, Amber, White
        float OutR, OutG, OutB, OutW;
        RGBtoRGBW(R, G, B, OutR, OutG, OutB, OutW);
        // Amber = R と G が重なる黄色成分 (min(R,G) が正しい)
        float Amber = FMath::Min(R, G);
        Channels.Add(Ch + 0, FloatToByte(OutR));
        Channels.Add(Ch + 1, FloatToByte(OutG));
        Channels.Add(Ch + 2, FloatToByte(OutB));
        Channels.Add(Ch + 3, FloatToByte(Amber));
        Channels.Add(Ch + 4, FloatToByte(OutW));
        break;
    }

    case ELightSyncColorMode::DRGB:
    {
        // 4ch: Dimmer, R, G, B
        float MaxComp = FMath::Max3(R, G, B);
        uint8 DimmerVal = FloatToByte(MaxComp);

        // Dimmerで正規化
        float NormFactor = (MaxComp > 0.001f) ? (1.0f / MaxComp) : 0.0f;
        Channels.Add(Ch + 0, DimmerVal);
        Channels.Add(Ch + 1, FloatToByte(FMath::Clamp(R * NormFactor, 0.0f, 1.0f)));
        Channels.Add(Ch + 2, FloatToByte(FMath::Clamp(G * NormFactor, 0.0f, 1.0f)));
        Channels.Add(Ch + 3, FloatToByte(FMath::Clamp(B * NormFactor, 0.0f, 1.0f)));
        break;
    }

    case ELightSyncColorMode::CCT:
    {
        // 2ch: CCT (色温度), Brightness (明度)
        float CCT, Brightness;
        RGBtoCCTBrightness(Mapping, R, G, B, CCT, Brightness);
        Channels.Add(Ch + 0, FloatToByte(CCT));
        Channels.Add(Ch + 1, FloatToByte(Brightness));
        break;
    }

    case ELightSyncColorMode::CCTRGBW10CH:
    {
        // 10ch: CCT & RGBW 8bit
        // Ch1: CCT (CCTMinKelvin - CCTMaxKelvin)
        // Ch2: Green/Magenta
        // Ch3: Dimmer
        // Ch4: Crossfade (CCT -> RGBW)
        // Ch5-8: R,G,B,W
        // Ch9: Strobe
        // Ch10: Reserved

        float CCT, Brightness;
        RGBtoCCTBrightness(Mapping, R, G, B, CCT, Brightness);

        float OutR, OutG, OutB, OutW;
        RGBtoRGBW(R, G, B, OutR, OutG, OutB, OutW);

        Channels.Add(Ch + 0, FloatToByte(CCT));
        Channels.Add(Ch + 1, static_cast<uint8>(FMath::Clamp(Mapping.CCTRGBW10_GreenMagenta, 0, 255)));
        Channels.Add(Ch + 2, FloatToByte(Brightness));
        Channels.Add(Ch + 3, static_cast<uint8>(FMath::Clamp(Mapping.CCTRGBW10_Crossfade, 0, 255)));
        Channels.Add(Ch + 4, FloatToByte(OutR));
        Channels.Add(Ch + 5, FloatToByte(OutG));
        Channels.Add(Ch + 6, FloatToByte(OutB));
        Channels.Add(Ch + 7, FloatToByte(OutW));
        Channels.Add(Ch + 8, static_cast<uint8>(FMath::Clamp(Mapping.CCTRGBW10_Strobe, 0, 255)));
        Channels.Add(Ch + 9, static_cast<uint8>(FMath::Clamp(Mapping.CCTRGBW10_Reserved10, 0, 255)));
        break;
    }
    }

    return Channels;
}

void UDMXColorOutputComponent::RGBtoRGBW(float R, float G, float B,
                                         float &OutR, float &OutG, float &OutB, float &OutW) const
{
    // White成分 = RGBの最小値
    OutW = FMath::Min3(R, G, B);

    // 残りの色成分
    OutR = R - OutW;
    OutG = G - OutW;
    OutB = B - OutW;
}

void UDMXColorOutputComponent::RGBtoCCTBrightness(const FDMXFixtureMapping &Mapping,
                                                  float R, float G, float B,
                                                  float &OutCCT, float &OutBrightness) const
{
    // リニアsRGB → CIE XYZ → xy色度 → McCamy の近似式で相関色温度を求め、
    // 灯体の CCT レンジ (CCTMinKelvin - CCTMaxKelvin) で 0-1 に正規化する。

    OutBrightness = FMath::Max3(R, G, B);

    const float MinK = FMath::Min(Mapping.CCTMinKelvin, Mapping.CCTMaxKelvin);
    const float MaxK = FMath::Max(Mapping.CCTMinKelvin, Mapping.CCTMaxKelvin);
    const float MidK = 0.5f * (MinK + MaxK);

    auto NormalizeKelvin = [MinK, MaxK](float Kelvin) -> float
    {
        return (MaxK - MinK) > 1.0f ? FMath::Clamp((Kelvin - MinK) / (MaxK - MinK), 0.0f, 1.0f) : 0.5f;
    };

    if (OutBrightness < 0.001f)
    {
        OutCCT = NormalizeKelvin(MidK); // デフォルト: 中間色温度
        OutBrightness = 0.0f;
        return;
    }

    const float X = 0.4124f * R + 0.3576f * G + 0.1805f * B;
    const float Y = 0.2126f * R + 0.7152f * G + 0.0722f * B;
    const float Z = 0.0193f * R + 0.1192f * G + 0.9505f * B;
    const float Sum = X + Y + Z;

    if (Sum < KINDA_SMALL_NUMBER)
    {
        OutCCT = NormalizeKelvin(MidK);
        return;
    }

    const float x = X / Sum;
    const float y = Y / Sum;
    const float Denom = 0.1858f - y;
    if (FMath::Abs(Denom) < KINDA_SMALL_NUMBER)
    {
        OutCCT = NormalizeKelvin(MidK);
        return;
    }

    // McCamy (1992)。黒体軌跡から大きく外れる飽和色では値が発散するので範囲を制限する
    const float n = (x - 0.3320f) / Denom;
    const float Kelvin = FMath::Clamp(449.0f * n * n * n + 3525.0f * n * n + 6823.3f * n + 5520.33f,
                                      1000.0f, 40000.0f);

    OutCCT = NormalizeKelvin(Kelvin);
}
