// Copyright UE-Comp. All Rights Reserved.

#include "OSCColorOutputComponent.h"
#include "LightSyncDMXModule.h"

#include "OSCClient.h"
#include "OSCMessage.h"
#include "OSCBundle.h"
#include "OSCManager.h"

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

void UOSCColorOutputComponent::InitializeClients()
{
    DestroyClients();

    for (int32 i = 0; i < Targets.Num(); ++i)
    {
        const FOSCTargetConfig &Config = Targets[i];

        if (!Config.bEnabled)
        {
            OSCClients.Add(nullptr);
            continue;
        }

        // UOSCClient を動的に生成
        UOSCClient *Client = UOSCManager::CreateOSCClient(
            Config.IPAddress,
            Config.Port,
            FString::Printf(TEXT("LightSync_OSC_%d"), i),
            this);

        if (Client)
        {
            OSCClients.Add(Client);
            UE_LOG(LogLightSyncDMX, Log, TEXT("OSC Client created: '%s' -> %s:%d"),
                   *Config.TargetName, *Config.IPAddress, Config.Port);
        }
        else
        {
            OSCClients.Add(nullptr);
            UE_LOG(LogLightSyncDMX, Warning,
                   TEXT("Failed to create OSC Client for target '%s' (%s:%d)"),
                   *Config.TargetName, *Config.IPAddress, Config.Port);
        }
    }

    bIsConnected = OSCClients.Num() > 0;
}

void UOSCColorOutputComponent::DestroyClients()
{
    OSCClients.Empty();
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

    LastSentColor = Color;

    for (int32 i = 0; i < Targets.Num(); ++i)
    {
        if (Targets[i].bEnabled)
        {
            SendToTarget(i, Color);
        }
    }

    SentMessageCount++;
}

void UOSCColorOutputComponent::SendColorToTarget(const FLinearColor &Color, int32 TargetIndex)
{
    if (!bOSCOutputEnabled || !Targets.IsValidIndex(TargetIndex))
    {
        return;
    }

    if (Targets[TargetIndex].bEnabled)
    {
        SendToTarget(TargetIndex, Color);
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

void UOSCColorOutputComponent::SendFormattedMessage(
    UOSCClient *Client, const FOSCTargetConfig &Config, const FLinearColor &Color)
{
    if (!Client)
    {
        return;
    }

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
        SendDasLightFormat(Client, Config, Color);
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
    UOSCClient *Client, const FOSCTargetConfig &Config, const FLinearColor &Color)
{
    if (!Client)
        return;

    // DasLight は /dmx/{universe}/{channel} [int 0-255] 形式
    const int32 R = FMath::RoundToInt(FMath::Clamp(Color.R, 0.0f, 1.0f) * 255);
    const int32 G = FMath::RoundToInt(FMath::Clamp(Color.G, 0.0f, 1.0f) * 255);
    const int32 B = FMath::RoundToInt(FMath::Clamp(Color.B, 0.0f, 1.0f) * 255);

    const int32 Univ = Config.DMXUniverse;
    const int32 Ch = Config.DMXStartChannel;

    if (bUseBundleMessage)
    {
        FOSCBundle Bundle;

        FOSCMessage MsgR;
        MsgR.SetAddress(FOSCAddress(*FString::Printf(TEXT("/dmx/%d/%d"), Univ, Ch)));
        UOSCManager::AddInt32(MsgR, R);
        UOSCManager::AddMessageToBundle(MsgR, Bundle);

        FOSCMessage MsgG;
        MsgG.SetAddress(FOSCAddress(*FString::Printf(TEXT("/dmx/%d/%d"), Univ, Ch + 1)));
        UOSCManager::AddInt32(MsgG, G);
        UOSCManager::AddMessageToBundle(MsgG, Bundle);

        FOSCMessage MsgB;
        MsgB.SetAddress(FOSCAddress(*FString::Printf(TEXT("/dmx/%d/%d"), Univ, Ch + 2)));
        UOSCManager::AddInt32(MsgB, B);
        UOSCManager::AddMessageToBundle(MsgB, Bundle);

        Client->SendOSCBundle(Bundle);
    }
    else
    {
        {
            FOSCMessage Msg;
            Msg.SetAddress(FOSCAddress(*FString::Printf(TEXT("/dmx/%d/%d"), Univ, Ch)));
            UOSCManager::AddInt32(Msg, R);
            Client->SendOSCMessage(Msg);
        }
        {
            FOSCMessage Msg;
            Msg.SetAddress(FOSCAddress(*FString::Printf(TEXT("/dmx/%d/%d"), Univ, Ch + 1)));
            UOSCManager::AddInt32(Msg, G);
            Client->SendOSCMessage(Msg);
        }
        {
            FOSCMessage Msg;
            Msg.SetAddress(FOSCAddress(*FString::Printf(TEXT("/dmx/%d/%d"), Univ, Ch + 2)));
            UOSCManager::AddInt32(Msg, B);
            Client->SendOSCMessage(Msg);
        }
    }
}

void UOSCColorOutputComponent::SendCustomOSCFloat(const FString &Address, float Value)
{
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
    SendColor(FLinearColor::Black);
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
    // クライアントが未作成でターゲットがある場合に初期化
    if (OSCClients.Num() == 0 && Targets.Num() > 0)
    {
        InitializeClients();
    }
}
