// Copyright UE-Comp. All Rights Reserved.

#include "OSCColorOutputComponent.h"
#include "LightSyncDMXModule.h"

#include "OSCClient.h"
#include "OSCMessage.h"
#include "OSCBundle.h"
#include "OSCManager.h"
#include "HAL/PlatformTime.h"

UOSCColorOutputComponent::UOSCColorOutputComponent()
{
    PrimaryComponentTick.bCanEverTick = false;

    // デフォルトターゲットを1つ追加
    FOSCTargetConfig DefaultTarget;
    DefaultTarget.TargetName = TEXT("QLC+ PC");
    DefaultTarget.IPAddress = TEXT("127.0.0.1");
    DefaultTarget.Port = 7700;
    DefaultTarget.Format = EOSCMessageFormat::FloatRGB;
    Targets.Add(DefaultTarget);
}

void UOSCColorOutputComponent::BeginPlay()
{
    Super::BeginPlay();
    InitializeClients();
}

void UOSCColorOutputComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Blackout();
    DestroyClients();
    Super::EndPlay(EndPlayReason);
}

FString UOSCColorOutputComponent::MakeEndpointKey(const FOSCTargetConfig &Config)
{
    return FString::Printf(TEXT("%s:%d"), *Config.IPAddress.TrimStartAndEnd(), Config.Port);
}

void UOSCColorOutputComponent::InitializeClients()
{
    DestroyClients();

    for (int32 i = 0; i < Targets.Num(); ++i)
    {
        const FOSCTargetConfig &Config = Targets[i];

        // 無効なターゲットもクライアントは作っておく (後から有効化したときに再接続不要にするため)
        // 名前は一意にする (同名で NewObject すると破棄待ちの旧クライアントを上書きしてしまう)
        const FString IPAddress = Config.IPAddress.TrimStartAndEnd();
        UOSCClient *Client = UOSCManager::CreateOSCClient(
            IPAddress,
            Config.Port,
            MakeUniqueObjectName(this, UOSCClient::StaticClass(), *FString::Printf(TEXT("LightSync_OSC_%d"), i)).ToString(),
            this);

        // CreateOSCClient は送信先の解析失敗を戻り値で返さないので、ここで再設定して判定する
        if (Client && Client->SetSendIPAddress(IPAddress, Config.Port))
        {
            OSCClients.Add(Client);
            UE_LOG(LogLightSyncDMX, Log, TEXT("OSC Client created: '%s' -> %s:%d"),
                   *Config.TargetName, *Config.IPAddress, Config.Port);
        }
        else
        {
            OSCClients.Add(nullptr);
            UE_LOG(LogLightSyncDMX, Warning,
                   TEXT("Failed to create OSC Client for target '%s' (%s:%d). IPアドレスを確認してください。"),
                   *Config.TargetName, *Config.IPAddress, Config.Port);
        }

        ClientEndpoints.Add(MakeEndpointKey(Config));
    }

    // 有効なクライアントが1つ以上あるときだけ接続済みとみなす
    bIsConnected = OSCClients.ContainsByPredicate([](const UOSCClient* C) { return C != nullptr; });
}

void UOSCColorOutputComponent::DestroyClients()
{
    // UOSCClient::Stop は protected。参照を外せば GC 時にソケットが閉じられる
    OSCClients.Empty();
    ClientEndpoints.Empty();
    bIsConnected = false;
}

void UOSCColorOutputComponent::SendColor(const FLinearColor &Color)
{
    if (!bOSCOutputEnabled)
    {
        return;
    }

    // エディタモードではBeginPlayが呼ばれないため、遅延初期化
    EnsureClientsInitialized();

    const double NowSeconds = FPlatformTime::Seconds();
    const double MinInterval = 1.0 / FMath::Max(static_cast<double>(OSCSendRate), 1.0);
    if (LastOSCSendTimeSeconds >= 0.0 && (NowSeconds - LastOSCSendTimeSeconds) < MinInterval)
    {
        return;
    }

    LastOSCSendTimeSeconds = NowSeconds;

    FLinearColor DimmedColor = Color * FMath::Clamp(MasterDimmer, 0.0f, 1.0f);
    DimmedColor.A = Color.A;
    LastSentColor = DimmedColor;

    for (int32 i = 0; i < Targets.Num(); ++i)
    {
        if (Targets[i].bEnabled)
        {
            SendToTarget(i, DimmedColor);
        }
    }

    SentMessageCount++;
}

void UOSCColorOutputComponent::SendZoneColors(const TArray<FLightSyncZoneResult> &ZoneResults)
{
    if (!bOSCOutputEnabled || ZoneResults.Num() == 0)
    {
        return;
    }

    EnsureClientsInitialized();

    const double NowSeconds = FPlatformTime::Seconds();
    const double MinInterval = 1.0 / FMath::Max(static_cast<double>(OSCSendRate), 1.0);
    if (LastOSCSendTimeSeconds >= 0.0 && (NowSeconds - LastOSCSendTimeSeconds) < MinInterval)
    {
        return;
    }
    LastOSCSendTimeSeconds = NowSeconds;

    for (int32 i = 0; i < Targets.Num(); ++i)
    {
        if (!Targets[i].bEnabled)
        {
            continue;
        }

        const FLightSyncZoneResult *Found = ZoneResults.FindByPredicate(
            [this, i](const FLightSyncZoneResult &Result) { return Result.Name == Targets[i].Zone; });
        if (!Found)
        {
            continue;
        }

        FLinearColor DimmedColor = Found->Color * FMath::Clamp(MasterDimmer, 0.0f, 1.0f);
        DimmedColor.A = 1.0f;
        LastSentColor = DimmedColor;
        SendToTarget(i, DimmedColor);
    }

    SentMessageCount++;
}

void UOSCColorOutputComponent::SendColorToTarget(const FLinearColor &Color, int32 TargetIndex)
{
    if (!bOSCOutputEnabled || !Targets.IsValidIndex(TargetIndex))
    {
        return;
    }

    EnsureClientsInitialized();

    if (Targets[TargetIndex].bEnabled)
    {
        FLinearColor DimmedColor = Color * FMath::Clamp(MasterDimmer, 0.0f, 1.0f);
        DimmedColor.A = Color.A;
        SendToTarget(TargetIndex, DimmedColor);
    }
}

void UOSCColorOutputComponent::SendToTarget(int32 TargetIndex, const FLinearColor &Color)
{
    if (!OSCClients.IsValidIndex(TargetIndex) || !OSCClients[TargetIndex])
    {
        return;
    }

    UOSCClient *Client = OSCClients[TargetIndex];
    const FOSCTargetConfig &Config = Targets[TargetIndex];

    SendFormattedMessage(Client, Config, Color);
}

void UOSCColorOutputComponent::ResolveTargetValues(const FOSCTargetConfig &Config, const FLinearColor &Color,
                                                   FLinearColor &OutRGB, TArray<uint8> &OutDMX) const
{
    const FLinearColor Target = Color * FMath::Max(Config.IntensityGain, 0.0f);

    // 色相を保って 1.0 以下に収める
    auto FitToUnit = [](FLinearColor C)
    {
        C.R = FMath::Max(C.R, 0.0f);
        C.G = FMath::Max(C.G, 0.0f);
        C.B = FMath::Max(C.B, 0.0f);
        const float Peak = FMath::Max3(C.R, C.G, C.B);
        if (Peak > 1.0f)
        {
            C /= Peak;
        }
        C.A = 1.0f;
        return C;
    };

    switch (Config.ValueMode)
    {
    case EOSCColorValue::FixtureLevels:
    {
        const FLightSyncFixtureProfileData Profile = Config.Profile ? Config.Profile->Profile
                                                                    : LightSyncFixture::MakePreset(ELightSyncColorMode::RGB);
        const LightSyncColor::FMatrix3x3 RefToCamera = StageCalibration ? StageCalibration->GetReferenceToCamera()
                                                                        : LightSyncColor::FMatrix3x3::Identity();
        const FLightSyncFixtureOutput Output = LightSyncFixture::Compute(Profile, Target, RefToCamera, 1.0);
        OutDMX = Output.Values;
        auto ValueAt = [&OutDMX](int32 Index) { return OutDMX.IsValidIndex(Index) ? OutDMX[Index] / 255.0f : 0.0f; };
        OutRGB = FLinearColor(ValueAt(0), ValueAt(1), ValueAt(2), 1.0f);
        return;
    }
    case EOSCColorValue::DisplayEncodedTarget:
    {
        // IEC 61966-2-1 sRGB OETF
        auto Encode = [](float V) { return V <= 0.0031308f ? 12.92f * V : 1.055f * FMath::Pow(V, 1.0f / 2.4f) - 0.055f; };
        const FLinearColor Linear = FitToUnit(Target);
        OutRGB = FLinearColor(Encode(Linear.R), Encode(Linear.G), Encode(Linear.B), 1.0f);
        break;
    }
    default:
        OutRGB = FitToUnit(Target);
        break;
    }

    OutDMX = {static_cast<uint8>(FMath::RoundToInt(OutRGB.R * 255.0f)),
              static_cast<uint8>(FMath::RoundToInt(OutRGB.G * 255.0f)),
              static_cast<uint8>(FMath::RoundToInt(OutRGB.B * 255.0f))};
}

void UOSCColorOutputComponent::SendFormattedMessage(
    UOSCClient *Client, const FOSCTargetConfig &Config, const FLinearColor &InColor)
{
    if (!Client)
    {
        return;
    }

    // ValueMode に従って送る値を決める (以降の Color は 0-1 の送信値)
    FLinearColor Color;
    TArray<uint8> DMXValues;
    ResolveTargetValues(Config, InColor, Color, DMXValues);

    // プローブ名をOSCアドレスセーフにする (スペース→アンダースコア)
    FString SafeProbeName = OSCProbeName;
    SafeProbeName.ReplaceCharInline(TEXT(' '), TEXT('_'));

    switch (Config.Format)
    {
    case EOSCMessageFormat::FloatRGB:
    {
        // /lightsync/probe01/rgb [float, float, float]
        FOSCAddress Address(*FString::Printf(TEXT("%s/%s/rgb"), *Config.AddressPrefix, *SafeProbeName));
        FOSCMessage Message;
        Message.SetAddress(Address);
        UOSCManager::AddFloat(Message, Color.R);
        UOSCManager::AddFloat(Message, Color.G);
        UOSCManager::AddFloat(Message, Color.B);
        Client->SendOSCMessage(Message);
        break;
    }

    case EOSCMessageFormat::IntRGB255:
    {
        // /lightsync/probe01/rgb [int, int, int]
        FOSCAddress Address(*FString::Printf(TEXT("%s/%s/rgb"), *Config.AddressPrefix, *SafeProbeName));
        FOSCMessage Message;
        Message.SetAddress(Address);
        UOSCManager::AddInt32(Message, FMath::RoundToInt(FMath::Clamp(Color.R, 0.0f, 1.0f) * 255));
        UOSCManager::AddInt32(Message, FMath::RoundToInt(FMath::Clamp(Color.G, 0.0f, 1.0f) * 255));
        UOSCManager::AddInt32(Message, FMath::RoundToInt(FMath::Clamp(Color.B, 0.0f, 1.0f) * 255));
        Client->SendOSCMessage(Message);
        break;
    }

    case EOSCMessageFormat::FloatRGBA:
    {
        // /lightsync/probe01/color [float, float, float, float]
        FOSCAddress Address(*FString::Printf(TEXT("%s/%s/color"), *Config.AddressPrefix, *SafeProbeName));
        FOSCMessage Message;
        Message.SetAddress(Address);
        UOSCManager::AddFloat(Message, Color.R);
        UOSCManager::AddFloat(Message, Color.G);
        UOSCManager::AddFloat(Message, Color.B);
        UOSCManager::AddFloat(Message, Color.A);
        Client->SendOSCMessage(Message);
        break;
    }

    case EOSCMessageFormat::QLCPlusCompat:
    {
        SendQLCPlusFormat(Client, Config, Color);
        break;
    }

    case EOSCMessageFormat::DasLightCompat:
    {
        SendDasLightFormat(Client, Config, DMXValues);
        break;
    }

    case EOSCMessageFormat::Custom:
    {
        // ユーザー定義アドレスに float3 で送信
        FOSCAddress Address(*Config.CustomAddress);
        FOSCMessage Message;
        Message.SetAddress(Address);
        UOSCManager::AddFloat(Message, Color.R);
        UOSCManager::AddFloat(Message, Color.G);
        UOSCManager::AddFloat(Message, Color.B);
        Client->SendOSCMessage(Message);
        break;
    }
    }

    UE_LOG(LogLightSyncDMX, VeryVerbose,
           TEXT("OSC Sent to '%s' (%s:%d): Color=(%.3f, %.3f, %.3f)"),
           *Config.TargetName, *Config.IPAddress, Config.Port,
           Color.R, Color.G, Color.B);
}

void UOSCColorOutputComponent::SendQLCPlusFormat(
    UOSCClient *Client, const FOSCTargetConfig &Config, const FLinearColor &Color)
{
    if (!Client)
        return;

    // QLC+ の OSC プラグインは以下のフォーマットに対応:
    // /{universe}/{channel} [int 0-255]
    // または個別チャンネルで R, G, B を送信

    const int32 R = FMath::RoundToInt(FMath::Clamp(Color.R, 0.0f, 1.0f) * 255);
    const int32 G = FMath::RoundToInt(FMath::Clamp(Color.G, 0.0f, 1.0f) * 255);
    const int32 B = FMath::RoundToInt(FMath::Clamp(Color.B, 0.0f, 1.0f) * 255);

    FString SafeProbeName = OSCProbeName;
    SafeProbeName.ReplaceCharInline(TEXT(' '), TEXT('_'));

    if (bUseBundleMessage)
    {
        // バンドルで3メッセージをまとめて送信
        FOSCBundle Bundle;

        // R チャンネル
        FOSCMessage MsgR;
        MsgR.SetAddress(FOSCAddress(*FString::Printf(TEXT("%s/%s/r"), *Config.AddressPrefix, *SafeProbeName)));
        UOSCManager::AddInt32(MsgR, R);
        UOSCManager::AddMessageToBundle(MsgR, Bundle);

        // G チャンネル
        FOSCMessage MsgG;
        MsgG.SetAddress(FOSCAddress(*FString::Printf(TEXT("%s/%s/g"), *Config.AddressPrefix, *SafeProbeName)));
        UOSCManager::AddInt32(MsgG, G);
        UOSCManager::AddMessageToBundle(MsgG, Bundle);

        // B チャンネル
        FOSCMessage MsgB;
        MsgB.SetAddress(FOSCAddress(*FString::Printf(TEXT("%s/%s/b"), *Config.AddressPrefix, *SafeProbeName)));
        UOSCManager::AddInt32(MsgB, B);
        UOSCManager::AddMessageToBundle(MsgB, Bundle);

        Client->SendOSCBundle(Bundle);
    }
    else
    {
        // 個別送信
        {
            FOSCMessage Msg;
            Msg.SetAddress(FOSCAddress(*FString::Printf(TEXT("%s/%s/r"), *Config.AddressPrefix, *SafeProbeName)));
            UOSCManager::AddInt32(Msg, R);
            Client->SendOSCMessage(Msg);
        }
        {
            FOSCMessage Msg;
            Msg.SetAddress(FOSCAddress(*FString::Printf(TEXT("%s/%s/g"), *Config.AddressPrefix, *SafeProbeName)));
            UOSCManager::AddInt32(Msg, G);
            Client->SendOSCMessage(Msg);
        }
        {
            FOSCMessage Msg;
            Msg.SetAddress(FOSCAddress(*FString::Printf(TEXT("%s/%s/b"), *Config.AddressPrefix, *SafeProbeName)));
            UOSCManager::AddInt32(Msg, B);
            Client->SendOSCMessage(Msg);
        }
    }
}

void UOSCColorOutputComponent::SendDasLightFormat(
    UOSCClient *Client, const FOSCTargetConfig &Config, const TArray<uint8> &Values)
{
    if (!Client)
        return;

    // DasLight は /dmx/{universe}/{channel} [int 0-255] 形式。プロファイルの全チャンネルを送る
    const int32 Univ = Config.DMXUniverse;
    const int32 Ch = Config.DMXStartChannel;

    auto MakeMessage = [Univ, Ch](int32 Offset, uint8 Value)
    {
        FOSCMessage Msg;
        Msg.SetAddress(FOSCAddress(*FString::Printf(TEXT("/dmx/%d/%d"), Univ, Ch + Offset)));
        UOSCManager::AddInt32(Msg, Value);
        return Msg;
    };

    if (bUseBundleMessage)
    {
        FOSCBundle Bundle;
        for (int32 Offset = 0; Offset < Values.Num(); ++Offset)
        {
            FOSCMessage Msg = MakeMessage(Offset, Values[Offset]);
            UOSCManager::AddMessageToBundle(Msg, Bundle);
        }
        Client->SendOSCBundle(Bundle);
    }
    else
    {
        for (int32 Offset = 0; Offset < Values.Num(); ++Offset)
        {
            FOSCMessage Msg = MakeMessage(Offset, Values[Offset]);
            Client->SendOSCMessage(Msg);
        }
    }
}

void UOSCColorOutputComponent::SendCustomOSCFloat(const FString &Address, float Value)
{
    EnsureClientsInitialized();

    for (int32 i = 0; i < OSCClients.Num(); ++i)
    {
        if (OSCClients[i] && Targets.IsValidIndex(i) && Targets[i].bEnabled)
        {
            FOSCMessage Msg;
            Msg.SetAddress(FOSCAddress(*Address));
            UOSCManager::AddFloat(Msg, Value);
            OSCClients[i]->SendOSCMessage(Msg);
        }
    }
}

void UOSCColorOutputComponent::SendCustomOSCInt(const FString &Address, int32 Value)
{
    EnsureClientsInitialized();

    for (int32 i = 0; i < OSCClients.Num(); ++i)
    {
        if (OSCClients[i] && Targets.IsValidIndex(i) && Targets[i].bEnabled)
        {
            FOSCMessage Msg;
            Msg.SetAddress(FOSCAddress(*Address));
            UOSCManager::AddInt32(Msg, Value);
            OSCClients[i]->SendOSCMessage(Msg);
        }
    }
}

void UOSCColorOutputComponent::Blackout()
{
    EnsureClientsInitialized();

    LastSentColor = FLinearColor::Black;

    for (int32 i = 0; i < Targets.Num(); ++i)
    {
        if (Targets[i].bEnabled)
        {
            SendToTarget(i, FLinearColor::Black);
        }
    }

    SentMessageCount++;
}

void UOSCColorOutputComponent::AddTarget(const FOSCTargetConfig &Config)
{
    Targets.Add(Config);
    // クライアントを再初期化
    ReconnectAll();
}

void UOSCColorOutputComponent::ReconnectAll()
{
    InitializeClients();
}

void UOSCColorOutputComponent::EnsureClientsInitialized()
{
    // ターゲット数や送信先が変わっていれば作り直す (エディタ/Blueprintからの直接編集に追従)
    bool bNeedsRebuild = (OSCClients.Num() != Targets.Num()) || (ClientEndpoints.Num() != Targets.Num());
    for (int32 i = 0; !bNeedsRebuild && i < Targets.Num(); ++i)
    {
        bNeedsRebuild = (ClientEndpoints[i] != MakeEndpointKey(Targets[i]));
    }

    if (bNeedsRebuild)
    {
        InitializeClients();
    }
}

#if WITH_EDITOR
void UOSCColorOutputComponent::PostEditChangeProperty(FPropertyChangedEvent &PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName MemberName = PropertyChangedEvent.GetMemberPropertyName();
    if (MemberName == GET_MEMBER_NAME_CHECKED(UOSCColorOutputComponent, Targets))
    {
        ReconnectAll();
    }
}
#endif
