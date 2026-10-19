// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncDirectOutput.h"
#include "LightSyncDMXModule.h"

#include "DMXProtocolModule.h"
#include "IO/DMXOutputPort.h"
#include "IO/DMXOutputPortConfig.h"
#include "IO/DMXPortManager.h"
#include "DMXProtocolTypes.h"

#include "Common/UdpSocketBuilder.h"
#include "IPAddress.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

namespace LightSyncDirectOutput
{
    namespace
    {
        constexpr int32 ArtNetPort = 6454;
        constexpr int32 SACNPort = 5568;

        /** ポート → ローカルユニバース → チャンネル → ブラックアウト値 */
        TMap<FGuid, TMap<int32, TMap<int32, uint8>>> GRecorded;

        void WriteBE16(TArray<uint8>& Out, int32 Offset, uint16 Value)
        {
            Out[Offset] = static_cast<uint8>(Value >> 8);
            Out[Offset + 1] = static_cast<uint8>(Value & 0xFF);
        }

        TSharedPtr<FInternetAddr> MakeAddr(const FString& IP, int32 Port)
        {
            ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
            TSharedRef<FInternetAddr> Addr = Subsystem->CreateInternetAddr();
            bool bValid = false;
            Addr->SetIp(*IP, bValid);
            if (!bValid)
            {
                return nullptr;
            }
            Addr->SetPort(Port);
            return Addr;
        }

        FSocket* MakeSocket(const FString& DeviceAddress, int32 BindPort)
        {
            FIPv4Address DeviceIP;
            FUdpSocketBuilder Builder(TEXT("LightSyncDirectBlackout"));
            Builder.AsNonBlocking().AsReusable().WithBroadcast();
            if (!DeviceAddress.IsEmpty() && FIPv4Address::Parse(DeviceAddress, DeviceIP))
            {
                // Art-Net は UE の送信ソケットと同じ送信元 (DeviceAddress:6454) にして、ノードで同じ送信元として扱わせる
                Builder.BoundToEndpoint(FIPv4Endpoint(DeviceIP, BindPort));
            }
            FSocket* Socket = Builder.Build();
            if (!Socket && BindPort != 0)
            {
                // 同じポートに束縛できない環境では送信元ポートは任意にする
                return MakeSocket(DeviceAddress, 0);
            }
            return Socket;
        }
    }

    TArray<uint8> MakeArtNetPacket(int32 PortAddress, const uint8 (&Data)[512])
    {
        TArray<uint8> Packet;
        Packet.SetNumZeroed(18 + 512);
        const char* Id = "Art-Net";
        FMemory::Memcpy(Packet.GetData(), Id, 8);   // 終端の 0 を含む 8 バイト
        Packet[8] = 0x00;                            // OpDmx = 0x5000 (リトルエンディアン)
        Packet[9] = 0x50;
        Packet[10] = 0;                              // ProtVer 14
        Packet[11] = 14;
        Packet[12] = 0;                              // Sequence (0 = 無効)
        Packet[13] = 0;                              // Physical
        Packet[14] = static_cast<uint8>(PortAddress & 0xFF);         // SubUni
        Packet[15] = static_cast<uint8>((PortAddress >> 8) & 0x7F);  // Net
        WriteBE16(Packet, 16, 512);
        FMemory::Memcpy(Packet.GetData() + 18, Data, 512);
        return Packet;
    }

    TArray<uint8> MakeSACNPacket(int32 Universe, uint8 Priority, uint8 Sequence, const FGuid& CID, const uint8 (&Data)[512])
    {
        constexpr int32 Size = 638;
        TArray<uint8> Packet;
        Packet.SetNumZeroed(Size);

        // Root layer
        WriteBE16(Packet, 0, 0x0010);
        WriteBE16(Packet, 2, 0x0000);
        const uint8 AcnId[12] = {0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00};
        FMemory::Memcpy(Packet.GetData() + 4, AcnId, 12);
        WriteBE16(Packet, 16, static_cast<uint16>(0x7000 | (Size - 16)));
        Packet[21] = 0x04;                                           // VECTOR_ROOT_E131_DATA
        const uint32 CidWords[4] = {CID.A, CID.B, CID.C, CID.D};
        for (int32 Word = 0; Word < 4; ++Word)
        {
            for (int32 Byte = 0; Byte < 4; ++Byte)
            {
                Packet[22 + Word * 4 + Byte] = static_cast<uint8>(CidWords[Word] >> (24 - 8 * Byte));
            }
        }

        // Framing layer
        WriteBE16(Packet, 38, static_cast<uint16>(0x7000 | (Size - 38)));
        Packet[43] = 0x02;                                           // VECTOR_E131_DATA_PACKET
        const char* Source = "LightSync DMX";
        FMemory::Memcpy(Packet.GetData() + 44, Source, FCStringAnsi::Strlen(Source));
        Packet[108] = Priority;
        Packet[111] = Sequence;
        Packet[112] = 0;                                             // Options
        WriteBE16(Packet, 113, static_cast<uint16>(Universe));

        // DMP layer
        WriteBE16(Packet, 115, static_cast<uint16>(0x7000 | (Size - 115)));
        Packet[117] = 0x02;                                          // VECTOR_DMP_SET_PROPERTY
        Packet[118] = 0xA1;
        WriteBE16(Packet, 119, 0x0000);
        WriteBE16(Packet, 121, 0x0001);
        WriteBE16(Packet, 123, 513);
        Packet[125] = 0x00;                                          // START コード
        FMemory::Memcpy(Packet.GetData() + 126, Data, 512);
        return Packet;
    }

    void RecordChannels(const FGuid& PortGuid, int32 LocalUniverse, const TMap<int32, uint8>& BlackoutValues)
    {
        TMap<int32, uint8>& Channels = GRecorded.FindOrAdd(PortGuid).FindOrAdd(LocalUniverse);
        Channels.Append(BlackoutValues);
    }

    void Reset()
    {
        GRecorded.Reset();
    }

    int32 SendBlackoutNow()
    {
        if (GRecorded.Num() == 0 || !ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
        {
            return 0;
        }

        static const FGuid CID = FGuid::NewGuid();
        int32 PacketsSent = 0;

        for (const FDMXOutputPortSharedRef& Port : FDMXPortManager::Get().GetOutputPorts())
        {
            const TMap<int32, TMap<int32, uint8>>* Universes = GRecorded.Find(Port->GetPortGuid());
            if (!Universes)
            {
                continue;
            }

            const FDMXOutputPortConfig Config = Port->MakeOutputPortConfig();
            const bool bArtNet = Config.GetProtocolName() == FDMXProtocolModule::DefaultProtocolArtNetName;
            const bool bSACN = Config.GetProtocolName() == FDMXProtocolModule::DefaultProtocolSACNName;
            if (!bArtNet && !bSACN)
            {
                continue;
            }

            FSocket* Socket = MakeSocket(Config.GetDeviceAddress(), bArtNet ? ArtNetPort : 0);
            if (!Socket)
            {
                UE_LOG(LogLightSyncDMX, Warning, TEXT("DirectBlackout: ソケットを作れませんでした (%s)"), *Config.GetPortName());
                continue;
            }

            for (const TPair<int32, TMap<int32, uint8>>& Universe : *Universes)
            {
                // ポートの最新のユニバース (他の送信元のチャンネルも含む) に、このプラグインのチャンネルだけブラックアウト値を重ねる
                uint8 Data[512];
                FMemory::Memzero(Data, sizeof(Data));
                FDMXSignalSharedPtr Signal;
                if (Port->GameThreadGetDMXSignal(Universe.Key, Signal, true) && Signal.IsValid())
                {
                    FMemory::Memcpy(Data, Signal->ChannelData.GetData(), FMath::Min(Signal->ChannelData.Num(), 512));
                }
                for (const TPair<int32, uint8>& Channel : Universe.Value)
                {
                    if (Channel.Key >= 1 && Channel.Key <= 512)
                    {
                        Data[Channel.Key - 1] = Channel.Value;
                    }
                }

                const int32 Extern = Port->ConvertLocalToExternUniverseID(Universe.Key);
                TArray<TSharedPtr<FInternetAddr>> Destinations;
                switch (Config.GetCommunicationType())
                {
                case EDMXCommunicationType::Broadcast:
                    Destinations.Add(MakeAddr(TEXT("255.255.255.255"), bArtNet ? ArtNetPort : SACNPort));
                    break;
                case EDMXCommunicationType::Multicast:
                    Destinations.Add(MakeAddr(FString::Printf(TEXT("239.255.%d.%d"), (Extern >> 8) & 0xFF, Extern & 0xFF), SACNPort));
                    break;
                default:
                    for (const FDMXOutputPortDestinationAddress& Address : Config.GetDestinationAddresses())
                    {
                        Destinations.Add(MakeAddr(Address.DestinationAddressString, bArtNet ? ArtNetPort : SACNPort));
                    }
                    break;
                }

                // 取りこぼし対策で数回送る
                for (int32 Repeat = 0; Repeat < 3; ++Repeat)
                {
                    const TArray<uint8> Packet = bArtNet ? MakeArtNetPacket(Extern, Data)
                                                         : MakeSACNPacket(Extern, static_cast<uint8>(FMath::Clamp(Config.GetPriority(), 0, 200)),
                                                                          static_cast<uint8>(Repeat), CID, Data);
                    for (const TSharedPtr<FInternetAddr>& Destination : Destinations)
                    {
                        int32 BytesSent = 0;
                        if (Destination.IsValid() && Socket->SendTo(Packet.GetData(), Packet.Num(), BytesSent, *Destination))
                        {
                            ++PacketsSent;
                        }
                    }
                }
            }

            Socket->Close();
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
        }

        UE_LOG(LogLightSyncDMX, Log, TEXT("DirectBlackout: 終了時のブラックアウトを直接送信しました (%d パケット)"), PacketsSent);
        return PacketsSent;
    }
}
