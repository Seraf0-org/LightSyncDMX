// Copyright UE-Comp. All Rights Reserved.

#include "DMXColorOutputComponent.h"
#include "LightSyncDMXModule.h"

#include "IO/DMXOutputPort.h"
#include "IO/DMXPortManager.h"
#include "DMXProtocolTypes.h"
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

void UDMXColorOutputComponent::SendDMXData(const FDMXFixtureMapping &Mapping, const FLinearColor &Color)
{
    // マッピングに基づいてチャンネルデータを構築
    TMap<int32, uint8> ChannelData = ColorToDMXChannels(Mapping, Color);

    if (ChannelData.Num() == 0)
    {
        return;
    }

    // UE 5.4: FDMXPortManager 経由で OutputPort を取得して送信
    FDMXPortManager &PortManager = FDMXPortManager::Get();
    const TArray<FDMXOutputPortSharedRef> &OutputPorts = PortManager.GetOutputPorts();

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

    // 明度スケーリング適用
    float R = FMath::Clamp(Color.R * Mapping.BrightnessScale * MasterDimmer, 0.0f, 1.0f);
    float G = FMath::Clamp(Color.G * Mapping.BrightnessScale * MasterDimmer, 0.0f, 1.0f);
    float B = FMath::Clamp(Color.B * Mapping.BrightnessScale * MasterDimmer, 0.0f, 1.0f);

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
        // Amber = R と G の混合成分
        float Amber = FMath::Min(R, G * 0.5f);
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
        RGBtoCCTBrightness(R, G, B, CCT, Brightness);
        Channels.Add(Ch + 0, FloatToByte(CCT));
        Channels.Add(Ch + 1, FloatToByte(Brightness));
        break;
    }

    case ELightSyncColorMode::CCTRGBW10CH:
    {
        // 10ch: CCT & RGBW 8bit
        // Ch1: CCT(2700K-7500K)
        // Ch2: Green/Magenta
        // Ch3: Dimmer
        // Ch4: Crossfade (CCT -> RGBW)
        // Ch5-8: R,G,B,W
        // Ch9: Strobe
        // Ch10: Reserved

        float CCT, Brightness;
        RGBtoCCTBrightness(R, G, B, CCT, Brightness);

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

void UDMXColorOutputComponent::RGBtoCCTBrightness(float R, float G, float B,
                                                  float &OutCCT, float &OutBrightness) const
{
    // McCamy's approximation を使用したRGB→CCT変換の簡易版
    // 実際の照明機器は 2700K (暖色) ～ 6500K (昼光色) 程度

    OutBrightness = FMath::Max3(R, G, B);

    if (OutBrightness < 0.001f)
    {
        OutCCT = 0.5f; // デフォルト: 中間色温度
        OutBrightness = 0.0f;
        return;
    }

    // 正規化
    float NR = R / OutBrightness;
    float NB = B / OutBrightness;

    // Blue/Red 比率で色温度を推定
    // 0.0 = 暖色 (2700K), 1.0 = 昼光色 (6500K)
    float Ratio = FMath::Clamp(NB / FMath::Max(NR, 0.001f), 0.0f, 2.0f);
    OutCCT = FMath::Clamp(Ratio / 2.0f, 0.0f, 1.0f);
}
