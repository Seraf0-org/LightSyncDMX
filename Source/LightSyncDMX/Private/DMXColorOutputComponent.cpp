// Copyright UE-Comp. All Rights Reserved.

#include "DMXColorOutputComponent.h"
#include "LightSyncDMXModule.h"

#include "IO/DMXOutputPort.h"
#include "IO/DMXPortManager.h"
#include "DMXProtocolTypes.h"
#include "DMXProtocolConstants.h"
#include "Library/DMXLibrary.h"
#include "LightSyncSettings.h"
#include "LightSyncDirectOutput.h"

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

// ============================================================
// プロファイル / 計算
// ============================================================

const FLightSyncFixtureProfileData &UDMXColorOutputComponent::GetMappingProfile(const FDMXFixtureMapping &Mapping)
{
    return Mapping.Profile ? Mapping.Profile->Profile : LightSyncFixture::GetPreset(Mapping.ColorMode);
}

int32 UDMXColorOutputComponent::GetChannelCount(ELightSyncColorMode Mode)
{
    return LightSyncFixture::GetPreset(Mode).GetChannelCount();
}

int32 UDMXColorOutputComponent::GetMappingChannelCount(const FDMXFixtureMapping &Mapping)
{
    return GetMappingProfile(Mapping).GetChannelCount();
}

bool UDMXColorOutputComponent::IsMappingInRange(const FDMXFixtureMapping &Mapping)
{
    const int32 Count = GetMappingChannelCount(Mapping);
    const int32 LastChannel = Mapping.StartChannel + Count - 1;
    return Count > 0 && Mapping.StartChannel >= 1 && LastChannel <= DMX_UNIVERSE_SIZE;
}

FLightSyncFixtureOutput UDMXColorOutputComponent::ComputeFixtureOutput(const FDMXFixtureMapping &Mapping, const FLinearColor &Color) const
{
    const LightSyncColor::FMatrix3x3 RefToCamera = StageCalibration ? StageCalibration->GetReferenceToCamera()
                                                                    : LightSyncColor::FMatrix3x3::Identity();
    const double Gain = static_cast<double>(Mapping.BrightnessScale) * FMath::Clamp(MasterDimmer, 0.0f, 1.0f);
    return LightSyncFixture::Compute(GetMappingProfile(Mapping), Color, RefToCamera, Gain);
}

TMap<int32, uint8> UDMXColorOutputComponent::ColorToDMXChannels(const FDMXFixtureMapping &Mapping, const FLinearColor &Color) const
{
    TMap<int32, uint8> Channels;
    const FLightSyncFixtureOutput Output = ComputeFixtureOutput(Mapping, Color);
    for (int32 Offset = 0; Offset < Output.Values.Num(); ++Offset)
    {
        Channels.Add(Mapping.StartChannel + Offset, Output.Values[Offset]);
    }
    return Channels;
}

// ============================================================
// 送信
// ============================================================

void UDMXColorOutputComponent::SendColor(const FLinearColor &Color)
{
    if (!bDMXOutputEnabled)
    {
        return;
    }

    LastSentColor = Color;
    for (int32 Index = 0; Index < FixtureMappings.Num(); ++Index)
    {
        if (FixtureMappings[Index].bEnabled)
        {
            SendFixture(Index, Color);
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

    for (int32 Index = 0; Index < FixtureMappings.Num(); ++Index)
    {
        const FDMXFixtureMapping &Mapping = FixtureMappings[Index];
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

        SendFixture(Index, Found->Color);
        LastSentColor = Found->Color;
    }

    SentFrameCount++;
}

void UDMXColorOutputComponent::SendColorToFixture(const FLinearColor &Color, int32 FixtureIndex)
{
    if (!bDMXOutputEnabled || !FixtureMappings.IsValidIndex(FixtureIndex) || !FixtureMappings[FixtureIndex].bEnabled)
    {
        return;
    }
    SendFixture(FixtureIndex, Color);
}

void UDMXColorOutputComponent::SendRawToFixture(int32 FixtureIndex, const TArray<uint8> &Values)
{
    if (FixtureMappings.IsValidIndex(FixtureIndex))
    {
        // テスト点灯はオペレーターの明示的な操作なので、送信役の判定を通さない
        SendValues(FixtureMappings[FixtureIndex], Values, nullptr, true);
    }
}

void UDMXColorOutputComponent::Blackout()
{
    // 安全動作なので enabled フラグに関係なく送る
    for (int32 Index = 0; Index < FixtureMappings.Num(); ++Index)
    {
        const FDMXFixtureMapping &Mapping = FixtureMappings[Index];
        const FLightSyncFixtureOutput Output = LightSyncFixture::ComputeBlackout(GetMappingProfile(Mapping));
        SendValues(Mapping, Output.Values, &Output.Values);

        if (FixtureStatus.IsValidIndex(Index))
        {
            FixtureStatus[Index].Intensity = 0.0f;
            FixtureStatus[Index].Values = Output.Values;
        }
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

void UDMXColorOutputComponent::SendFixture(int32 MappingIndex, const FLinearColor &Color)
{
    const FDMXFixtureMapping &Mapping = FixtureMappings[MappingIndex];
    const FLightSyncFixtureOutput Output = ComputeFixtureOutput(Mapping, Color);
    const FLightSyncFixtureOutput BlackoutOutput = LightSyncFixture::ComputeBlackout(GetMappingProfile(Mapping));
    SendValues(Mapping, Output.Values, &BlackoutOutput.Values);

    if (FixtureStatus.Num() != FixtureMappings.Num())
    {
        FixtureStatus.SetNum(FixtureMappings.Num());
    }
    FLightSyncFixtureStatus &Status = FixtureStatus[MappingIndex];
    Status.FixtureName = Mapping.FixtureName;
    Status.bOutOfGamut = Output.bOutOfGamut;
    Status.bClipped = Output.bClipped;
    Status.Intensity = static_cast<float>(Output.Intensity);
    Status.CCT = static_cast<float>(Output.CCT);
    Status.Values = Output.Values;
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

void UDMXColorOutputComponent::SendValues(const FDMXFixtureMapping &Mapping, const TArray<uint8> &Values,
                                          const TArray<uint8> *BlackoutValues, bool bBypassAuthority)
{
    // nDisplay クラスタ等で、送信役でない PC からは送らない (二重送信の防止)
    if (!bBypassAuthority && !LightSyncOutput::CanOutput(GetWorld()))
    {
        return;
    }

    // ユニバース (512ch) をはみ出すマッピングは送信しない (隣のユニバースや無関係なchを汚さないため)
    if (!IsMappingInRange(Mapping))
    {
        if (!WarnedOutOfRangeFixtures.Contains(Mapping.FixtureName))
        {
            UE_LOG(LogLightSyncDMX, Warning,
                   TEXT("DMXColorOutput: フィクスチャ '%s' は Ch%d から %dch 使用するため 512ch を超えます。送信をスキップします。"),
                   *Mapping.FixtureName, Mapping.StartChannel, GetMappingChannelCount(Mapping));
            WarnedOutOfRangeFixtures.Add(Mapping.FixtureName);
        }
        return;
    }
    WarnedOutOfRangeFixtures.Remove(Mapping.FixtureName);

    if (Values.Num() == 0)
    {
        return;
    }

    TMap<int32, uint8> ChannelData;
    const int32 Count = FMath::Min(Values.Num(), GetMappingChannelCount(Mapping));
    for (int32 Offset = 0; Offset < Count; ++Offset)
    {
        ChannelData.Add(Mapping.StartChannel + Offset, Values[Offset]);
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

    TMap<int32, uint8> BlackoutData;
    if (BlackoutValues)
    {
        for (int32 Offset = 0; Offset < FMath::Min(BlackoutValues->Num(), Count); ++Offset)
        {
            BlackoutData.Add(Mapping.StartChannel + Offset, (*BlackoutValues)[Offset]);
        }
    }

    for (const FDMXOutputPortSharedRef &OutputPort : OutputPorts)
    {
        OutputPort->SendDMX(Mapping.Universe, ChannelData);
        if (BlackoutData.Num() > 0)
        {
            LightSyncDirectOutput::RecordChannels(OutputPort->GetPortGuid(), Mapping.Universe, BlackoutData);
        }
    }

    UE_LOG(LogLightSyncDMX, VeryVerbose, TEXT("DMX Sent: Fixture='%s' Univ=%d Ch=%d (%d ch)"),
           *Mapping.FixtureName, Mapping.Universe, Mapping.StartChannel, ChannelData.Num());
}
