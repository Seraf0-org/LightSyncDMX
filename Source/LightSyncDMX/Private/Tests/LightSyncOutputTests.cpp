// Copyright UE-Comp. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "LightSyncSettings.h"
#include "LightSyncDirectOutput.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace LightSyncOutputTests
{
    /** コマンドラインと設定を一時的に差し替えて判定する */
    bool Evaluate(const TCHAR* CommandLine, ELightSyncOutputPolicy Policy, const FString& OutputNode, FString& OutReason)
    {
        ULightSyncSettings* Settings = GetMutableDefault<ULightSyncSettings>();
        const ELightSyncOutputPolicy SavedPolicy = Settings->OutputPolicy;
        const FString SavedNode = Settings->OutputClusterNodeId;
        const FString SavedCommandLine = FCommandLine::Get();

        Settings->OutputPolicy = Policy;
        Settings->OutputClusterNodeId = OutputNode;
        FCommandLine::Set(CommandLine);
        LightSyncOutput::InvalidateCache();
        const bool bResult = LightSyncOutput::CanOutput(nullptr, &OutReason);

        FCommandLine::Set(*SavedCommandLine);
        Settings->OutputPolicy = SavedPolicy;
        Settings->OutputClusterNodeId = SavedNode;
        LightSyncOutput::InvalidateCache();
        return bResult;
    }
}

using namespace LightSyncOutputTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncOutputPolicyTest, "LightSync.Output.WhichPCSends",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncOutputPolicyTest::RunTest(const FString& Parameters)
{
    FString Reason;
    const TCHAR* Editor = TEXT("Project.uproject");
    const TCHAR* Node0 = TEXT("Project.uproject -game -dc_cluster -dc_node=Node_0");
    const TCHAR* Node1 = TEXT("Project.uproject -game -dc_cluster -dc_node=Node_1");

    // Auto: エディタ / 単体実行は送る。クラスタは送信ノードだけ
    TestTrue(TEXT("Auto: editor sends"), Evaluate(Editor, ELightSyncOutputPolicy::Auto, TEXT(""), Reason));
    TestTrue(TEXT("Auto: output node sends"), Evaluate(Node0, ELightSyncOutputPolicy::Auto, TEXT("Node_0"), Reason));
    TestFalse(TEXT("Auto: other node silent"), Evaluate(Node1, ELightSyncOutputPolicy::Auto, TEXT("Node_0"), Reason));
    TestFalse(TEXT("Auto: unknown primary → silent"), Evaluate(Node1, ELightSyncOutputPolicy::Auto, TEXT(""), Reason));

    // ClusterNodeOnly: エディタからは送らない
    TestFalse(TEXT("ClusterNodeOnly: editor silent"), Evaluate(Editor, ELightSyncOutputPolicy::ClusterNodeOnly, TEXT("Node_0"), Reason));
    TestTrue(TEXT("ClusterNodeOnly: output node sends"), Evaluate(Node0, ELightSyncOutputPolicy::ClusterNodeOnly, TEXT("node_0"), Reason));

    // NonClusterOnly: クラスタノードからは送らない
    TestTrue(TEXT("NonClusterOnly: editor sends"), Evaluate(Editor, ELightSyncOutputPolicy::NonClusterOnly, TEXT(""), Reason));
    TestFalse(TEXT("NonClusterOnly: node silent"), Evaluate(Node0, ELightSyncOutputPolicy::NonClusterOnly, TEXT("Node_0"), Reason));

    // Disabled
    TestFalse(TEXT("Disabled"), Evaluate(Editor, ELightSyncOutputPolicy::Disabled, TEXT(""), Reason));

    // コマンドラインの上書きが最優先 (PC ごとに決める)
    TestTrue(TEXT("-LightSyncOutput=On overrides Disabled"),
             Evaluate(TEXT("Project.uproject -LightSyncOutput=On"), ELightSyncOutputPolicy::Disabled, TEXT(""), Reason));
    TestFalse(TEXT("-LightSyncOutput=Off overrides Auto"),
              Evaluate(TEXT("Project.uproject -LightSyncOutput=Off"), ELightSyncOutputPolicy::Auto, TEXT(""), Reason));
    TestTrue(TEXT("-LightSyncOutput=1 on a non-output node"),
             Evaluate(TEXT("Project.uproject -dc_cluster -dc_node=Node_3 -LightSyncOutput=1"), ELightSyncOutputPolicy::Auto, TEXT("Node_0"), Reason));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FLightSyncDirectPacketTest, "LightSync.Output.DirectBlackoutPackets",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FLightSyncDirectPacketTest::RunTest(const FString& Parameters)
{
    uint8 Data[512];
    for (int32 Index = 0; Index < 512; ++Index)
    {
        Data[Index] = static_cast<uint8>(Index & 0xFF);
    }

    // Art-Net ArtDmx (仕様: Art-Net 4)
    const TArray<uint8> Art = LightSyncDirectOutput::MakeArtNetPacket(0x123, Data);
    TestEqual(TEXT("ArtDmx size"), Art.Num(), 530);
    TestTrue(TEXT("ID"), FMemory::Memcmp(Art.GetData(), "Art-Net\0", 8) == 0);
    TestTrue(TEXT("OpDmx 0x5000 LE"), Art[8] == 0x00 && Art[9] == 0x50);
    TestTrue(TEXT("ProtVer 14"), Art[10] == 0 && Art[11] == 14);
    TestTrue(TEXT("SubUni / Net"), Art[14] == 0x23 && Art[15] == 0x01);
    TestTrue(TEXT("Length 512 BE"), Art[16] == 0x02 && Art[17] == 0x00);
    TestTrue(TEXT("data"), Art[18] == 0 && Art[18 + 300] == static_cast<uint8>(300 & 0xFF));

    // sACN E1.31
    const FGuid CID(0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10);
    const TArray<uint8> Acn = LightSyncDirectOutput::MakeSACNPacket(7, 100, 5, CID, Data);
    TestEqual(TEXT("E1.31 size"), Acn.Num(), 638);
    TestTrue(TEXT("preamble"), Acn[0] == 0x00 && Acn[1] == 0x10);
    TestTrue(TEXT("ACN id"), FMemory::Memcmp(Acn.GetData() + 4, "ASC-E1.17\0\0\0", 12) == 0);
    TestTrue(TEXT("root flags+len"), Acn[16] == 0x72 && Acn[17] == 0x6E);       // 0x7000 | 622
    TestTrue(TEXT("root vector 4"), Acn[21] == 0x04);
    TestTrue(TEXT("CID"), Acn[22] == 0x01 && Acn[37] == 0x10);
    TestTrue(TEXT("framing flags+len"), Acn[38] == 0x72 && Acn[39] == 0x58);    // 0x7000 | 600
    TestTrue(TEXT("framing vector 2"), Acn[43] == 0x02);
    TestTrue(TEXT("priority"), Acn[108] == 100);
    TestTrue(TEXT("sequence"), Acn[111] == 5);
    TestTrue(TEXT("universe BE"), Acn[113] == 0x00 && Acn[114] == 0x07);
    TestTrue(TEXT("dmp flags+len"), Acn[115] == 0x72 && Acn[116] == 0x0B);      // 0x7000 | 523
    TestTrue(TEXT("dmp vector/type"), Acn[117] == 0x02 && Acn[118] == 0xA1);
    TestTrue(TEXT("count 513"), Acn[123] == 0x02 && Acn[124] == 0x01);
    TestTrue(TEXT("start code + data"), Acn[125] == 0x00 && Acn[126 + 300] == static_cast<uint8>(300 & 0xFF));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
