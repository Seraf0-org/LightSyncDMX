// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncSettings.h"
#include "LightSyncNDisplay.h"
#include "LightSyncDMXModule.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

ULightSyncSettings::ULightSyncSettings()
{
    CategoryName = TEXT("Plugins");
    SectionName = TEXT("LightSync DMX");
}

namespace LightSyncOutput
{
    namespace
    {
        struct FCache
        {
            double Time = -1.0;
            bool bResult = true;
            FString Reason;
        };
        // エディタと PIE のワールドが交互に問い合わせても作り直さないよう、ワールドごとに持つ
        TMap<TWeakObjectPtr<const UWorld>, FCache> GCaches;

        /** -LightSyncOutput=On/Off (1/0, true/false も可)。指定がなければ -1 */
        int32 CommandLineOverride()
        {
            FString Value;
            if (!FParse::Value(FCommandLine::Get(), TEXT("LightSyncOutput="), Value))
            {
                return -1;
            }
            Value.TrimStartAndEndInline();
            if (Value.Equals(TEXT("On"), ESearchCase::IgnoreCase) || Value == TEXT("1") || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase))
            {
                return 1;
            }
            if (Value.Equals(TEXT("Off"), ESearchCase::IgnoreCase) || Value == TEXT("0") || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
            {
                return 0;
            }
            return -1;
        }

        /** 設定データの Cluster.PrimaryNode.Id (リフレクション) */
        FString GetPrimaryNodeId(const UWorld* World)
        {
            const AActor* Root = LightSyncNDisplay::FindRootActor(const_cast<UWorld*>(World));
            if (!Root)
            {
                return FString();
            }
            const FObjectPropertyBase* ConfigProp = CastField<FObjectPropertyBase>(Root->GetClass()->FindPropertyByName(TEXT("CurrentConfigData")));
            UObject* Config = ConfigProp ? ConfigProp->GetObjectPropertyValue_InContainer(Root) : nullptr;
            const FObjectPropertyBase* ClusterProp = Config ? CastField<FObjectPropertyBase>(Config->GetClass()->FindPropertyByName(TEXT("Cluster"))) : nullptr;
            UObject* Cluster = ClusterProp ? ClusterProp->GetObjectPropertyValue_InContainer(Config) : nullptr;
            const FStructProperty* PrimaryProp = Cluster ? CastField<FStructProperty>(Cluster->GetClass()->FindPropertyByName(TEXT("PrimaryNode"))) : nullptr;
            if (!PrimaryProp)
            {
                return FString();
            }
            const void* Primary = PrimaryProp->ContainerPtrToValuePtr<void>(Cluster);
            const FStrProperty* IdProp = CastField<FStrProperty>(PrimaryProp->Struct->FindPropertyByName(TEXT("Id")));
            return IdProp ? IdProp->GetPropertyValue_InContainer(Primary) : FString();
        }

        bool Evaluate(const UWorld* World, FString& OutReason)
        {
            const int32 Override = CommandLineOverride();
            if (Override >= 0)
            {
                OutReason = Override ? TEXT("コマンドライン -LightSyncOutput=On") : TEXT("コマンドライン -LightSyncOutput=Off");
                return Override == 1;
            }

            const ULightSyncSettings* Settings = GetDefault<ULightSyncSettings>();
            FString NodeId;
            const bool bClusterNode = IsClusterNode(&NodeId);

            switch (Settings->OutputPolicy)
            {
            case ELightSyncOutputPolicy::Disabled:
                OutReason = TEXT("Project Settings で送信が無効");
                return false;

            case ELightSyncOutputPolicy::NonClusterOnly:
                OutReason = bClusterNode ? TEXT("クラスタノードからは送らない設定") : TEXT("エディタ / 単体実行から送る設定");
                return !bClusterNode;

            case ELightSyncOutputPolicy::ClusterNodeOnly:
            case ELightSyncOutputPolicy::Auto:
            default:
                break;
            }

            if (!bClusterNode)
            {
                const bool bAllowed = Settings->OutputPolicy == ELightSyncOutputPolicy::Auto;
                OutReason = bAllowed ? TEXT("エディタ / 単体実行 (Auto)") : TEXT("クラスタノードだけが送る設定");
                return bAllowed;
            }

            FString OutputNode = Settings->OutputClusterNodeId;
            if (OutputNode.IsEmpty())
            {
                OutputNode = GetPrimaryNodeId(World);
            }
            if (OutputNode.IsEmpty())
            {
                OutReason = FString::Printf(TEXT("クラスタノード '%s': 送信ノードを特定できないため送らない (Output Cluster Node Id を設定してください)"), *NodeId);
                return false;
            }

            const bool bIsOutputNode = NodeId.Equals(OutputNode, ESearchCase::IgnoreCase);
            OutReason = FString::Printf(TEXT("クラスタノード '%s' (送信ノード: '%s')"), *NodeId, *OutputNode);
            return bIsOutputNode;
        }
    }

    bool IsClusterNode(FString* OutNodeId)
    {
        if (!FParse::Param(FCommandLine::Get(), TEXT("dc_cluster")))
        {
            return false;
        }
        if (OutNodeId)
        {
            FParse::Value(FCommandLine::Get(), TEXT("dc_node="), *OutNodeId);
        }
        return true;
    }

    bool CanOutput(const UWorld* World, FString* OutReason)
    {
        // RootActor の検索を毎フレームしないよう 1 秒キャッシュ
        const double Now = FPlatformTime::Seconds();
        check(IsInGameThread());
        if (GCaches.Num() > 8)
        {
            for (auto It = GCaches.CreateIterator(); It; ++It)
            {
                if (!It.Key().IsValid())
                {
                    It.RemoveCurrent();
                }
            }
        }

        FCache& Cache = GCaches.FindOrAdd(World);
        if (Cache.Time < 0.0 || Now - Cache.Time > 1.0)
        {
            const bool bPrevious = Cache.bResult;
            const bool bFirst = Cache.Time < 0.0;
            Cache.Time = Now;
            Cache.bResult = Evaluate(World, Cache.Reason);
            if (bFirst || bPrevious != Cache.bResult)
            {
                UE_LOG(LogLightSyncDMX, Log, TEXT("LightSync 出力: %s (%s)"), Cache.bResult ? TEXT("この PC から送信する") : TEXT("この PC からは送信しない"), *Cache.Reason);
            }
        }
        if (OutReason)
        {
            *OutReason = Cache.Reason;
        }
        return Cache.bResult;
    }

    void InvalidateCache()
    {
        GCaches.Reset();
    }
}
