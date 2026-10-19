// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncNDisplay.h"
#include "LightSyncOCIO.h"
#include "OpenColorIOColorSpace.h"
#include "LightSyncDMXModule.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

namespace LightSyncNDisplay
{
    // ============================================================
    // リフレクションのヘルパー
    // ============================================================

    namespace
    {
        const FProperty* FindProp(const UStruct* Struct, FName Name)
        {
            return Struct ? Struct->FindPropertyByName(Name) : nullptr;
        }

        /** 構造体メンバー (FStructProperty) のアドレスと型 */
        const void* GetStructMember(const UStruct* OwnerType, const void* Owner, FName Name, const UScriptStruct*& OutType)
        {
            OutType = nullptr;
            const FStructProperty* Prop = CastField<FStructProperty>(FindProp(OwnerType, Name));
            if (!Prop || !Owner)
            {
                return nullptr;
            }
            OutType = Prop->Struct;
            return Prop->ContainerPtrToValuePtr<void>(Owner);
        }

        bool GetBool(const UStruct* Type, const void* Container, FName Name, bool bDefault = false)
        {
            const FBoolProperty* Prop = CastField<FBoolProperty>(FindProp(Type, Name));
            return Prop ? Prop->GetPropertyValue_InContainer(Container) : bDefault;
        }

        float GetFloat(const UStruct* Type, const void* Container, FName Name, float Default)
        {
            const FFloatProperty* Prop = CastField<FFloatProperty>(FindProp(Type, Name));
            return Prop ? Prop->GetPropertyValue_InContainer(Container) : Default;
        }

        UObject* GetObject(const UStruct* Type, const void* Container, FName Name)
        {
            const FObjectPropertyBase* Prop = CastField<FObjectPropertyBase>(FindProp(Type, Name));
            return Prop ? Prop->GetObjectPropertyValue_InContainer(Container) : nullptr;
        }

        /** 数値 (float / double / FVector4 / FLinearColor / FVector / FVector2D) を最大 4 成分で読む */
        int32 ReadNumeric(const FProperty* Prop, const void* ValuePtr, double (&Out)[4])
        {
            if (const FFloatProperty* F = CastField<FFloatProperty>(Prop))
            {
                Out[0] = F->GetPropertyValue(ValuePtr);
                return 1;
            }
            if (const FDoubleProperty* D = CastField<FDoubleProperty>(Prop))
            {
                Out[0] = D->GetPropertyValue(ValuePtr);
                return 1;
            }
            if (const FStructProperty* S = CastField<FStructProperty>(Prop))
            {
                if (S->Struct == TBaseStructure<FVector4>::Get())
                {
                    const FVector4& V = *static_cast<const FVector4*>(ValuePtr);
                    Out[0] = V.X; Out[1] = V.Y; Out[2] = V.Z; Out[3] = V.W;
                    return 4;
                }
                if (S->Struct == TBaseStructure<FLinearColor>::Get())
                {
                    const FLinearColor& C = *static_cast<const FLinearColor*>(ValuePtr);
                    Out[0] = C.R; Out[1] = C.G; Out[2] = C.B; Out[3] = C.A;
                    return 4;
                }
                if (S->Struct == TBaseStructure<FVector>::Get())
                {
                    const FVector& V = *static_cast<const FVector*>(ValuePtr);
                    Out[0] = V.X; Out[1] = V.Y; Out[2] = V.Z;
                    return 3;
                }
                if (S->Struct == TBaseStructure<FVector2D>::Get())
                {
                    const FVector2D& V = *static_cast<const FVector2D*>(ValuePtr);
                    Out[0] = V.X; Out[1] = V.Y;
                    return 2;
                }
            }
            return 0;
        }

        void WriteNumeric(const FProperty* Prop, void* ValuePtr, const double (&In)[4])
        {
            if (const FFloatProperty* F = CastField<FFloatProperty>(Prop))
            {
                F->SetPropertyValue(ValuePtr, static_cast<float>(In[0]));
            }
            else if (const FDoubleProperty* D = CastField<FDoubleProperty>(Prop))
            {
                D->SetPropertyValue(ValuePtr, In[0]);
            }
            else if (const FStructProperty* S = CastField<FStructProperty>(Prop))
            {
                if (S->Struct == TBaseStructure<FVector4>::Get())
                {
                    *static_cast<FVector4*>(ValuePtr) = FVector4(In[0], In[1], In[2], In[3]);
                }
                else if (S->Struct == TBaseStructure<FLinearColor>::Get())
                {
                    *static_cast<FLinearColor*>(ValuePtr) = FLinearColor(In[0], In[1], In[2], In[3]);
                }
                else if (S->Struct == TBaseStructure<FVector>::Get())
                {
                    *static_cast<FVector*>(ValuePtr) = FVector(In[0], In[1], In[2]);
                }
                else if (S->Struct == TBaseStructure<FVector2D>::Get())
                {
                    *static_cast<FVector2D*>(ValuePtr) = FVector2D(In[0], In[1]);
                }
            }
        }

        const FBoolProperty* OverrideFlagFor(const UStruct* Type, FName ValueName)
        {
            return CastField<FBoolProperty>(FindProp(Type, FName(*(TEXT("bOverride_") + ValueName.ToString()))));
        }

        void SetOverrideFlag(FPostProcessSettings& PP, FName ValueName)
        {
            if (const FBoolProperty* Flag = OverrideFlagFor(FPostProcessSettings::StaticStruct(), ValueName))
            {
                Flag->SetPropertyValue_InContainer(&PP, true);
            }
        }
    }

    // ============================================================
    // FSceneView::OverridePostProcessSettings と同じ合成
    // ============================================================

    void OverridePostProcess(FPostProcessSettings& Dest, const FPostProcessSettings& Src, float Weight)
    {
        if (Weight <= 0.0f)
        {
            return;
        }
        Weight = FMath::Min(Weight, 1.0f);

        const UScriptStruct* Type = FPostProcessSettings::StaticStruct();
        for (TFieldIterator<FBoolProperty> It(Type); It; ++It)
        {
            const FString FlagName = It->GetName();
            if (!FlagName.StartsWith(TEXT("bOverride_")) || !It->GetPropertyValue_InContainer(&Src))
            {
                continue;
            }

            const FProperty* Value = FindProp(Type, FName(*FlagName.RightChop(10)));
            if (!Value)
            {
                continue;
            }

            void* DestPtr = Value->ContainerPtrToValuePtr<void>(&Dest);
            const void* SrcPtr = Value->ContainerPtrToValuePtr<void>(&Src);

            double D[4] = {0, 0, 0, 0}, S[4] = {0, 0, 0, 0};
            const int32 Num = ReadNumeric(Value, DestPtr, D);
            if (Num > 0 && ReadNumeric(Value, SrcPtr, S) == Num)
            {
                for (int32 Index = 0; Index < Num; ++Index)
                {
                    D[Index] = FMath::Lerp(D[Index], S[Index], static_cast<double>(Weight));
                }
                WriteNumeric(Value, DestPtr, D);
            }
            else
            {
                // enum / bool / オブジェクト等は置き換え (エンジンの SET_PP と同じ)
                Value->CopyCompleteValue(DestPtr, SrcPtr);
            }
            It->SetPropertyValue_InContainer(&Dest, true);
        }
    }

    // ============================================================
    // nDisplay の設定読み取り
    // ============================================================

    bool IsRootActor(const AActor* Actor)
    {
        for (const UClass* Class = Actor ? Actor->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
        {
            if (Class->GetFName() == TEXT("DisplayClusterRootActor"))
            {
                return true;
            }
        }
        return false;
    }

    AActor* FindRootActor(UWorld* World)
    {
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (IsRootActor(*It))
            {
                return *It;
            }
        }
        return nullptr;
    }

    namespace
    {
        UObject* GetConfigData(const AActor* RootActor)
        {
            return IsRootActor(RootActor) ? GetObject(RootActor->GetClass(), RootActor, TEXT("CurrentConfigData")) : nullptr;
        }

        /** Cluster.Nodes[*].Viewports を (ノード順, ビューポート ID, ビューポートオブジェクト) で列挙 */
        void ForEachViewport(UObject* ConfigData, TFunctionRef<void(const FString&, UObject*)> Visit)
        {
            UObject* Cluster = ConfigData ? GetObject(ConfigData->GetClass(), ConfigData, TEXT("Cluster")) : nullptr;
            const FMapProperty* Nodes = Cluster ? CastField<FMapProperty>(FindProp(Cluster->GetClass(), TEXT("Nodes"))) : nullptr;
            if (!Nodes)
            {
                return;
            }

            FScriptMapHelper NodeMap(Nodes, Nodes->ContainerPtrToValuePtr<void>(Cluster));
            for (FScriptMapHelper::FIterator NodeIt(NodeMap); NodeIt; ++NodeIt)
            {
                const FObjectPropertyBase* NodeValue = CastField<FObjectPropertyBase>(NodeMap.GetValueProperty());
                UObject* Node = NodeValue ? NodeValue->GetObjectPropertyValue(NodeMap.GetValuePtr(NodeIt)) : nullptr;
                const FMapProperty* Viewports = Node ? CastField<FMapProperty>(FindProp(Node->GetClass(), TEXT("Viewports"))) : nullptr;
                if (!Viewports)
                {
                    continue;
                }

                FScriptMapHelper ViewportMap(Viewports, Viewports->ContainerPtrToValuePtr<void>(Node));
                for (FScriptMapHelper::FIterator VpIt(ViewportMap); VpIt; ++VpIt)
                {
                    const FString* Id = static_cast<const FString*>(static_cast<const void*>(ViewportMap.GetKeyPtr(VpIt)));
                    const FObjectPropertyBase* VpValue = CastField<FObjectPropertyBase>(ViewportMap.GetValueProperty());
                    UObject* Viewport = VpValue ? VpValue->GetObjectPropertyValue(ViewportMap.GetValuePtr(VpIt)) : nullptr;
                    if (Id && Viewport)
                    {
                        Visit(*Id, Viewport);
                    }
                }
            }
        }

        /** カラーグレーディング 1 枚 */
        struct FGradingRef
        {
            const UScriptStruct* Type = nullptr;
            const void* Data = nullptr;
        };

        /** ImplBlendPostProcessSettings の 1 項目 */
        enum class EBlendOp : uint8 { Add, Multiply, Override };
        struct FGradingField
        {
            const TCHAR* Group;    // ColorGradingRenderingSettings 内のグループ ("" = 直下)
            const TCHAR* Name;     // グループ内の名前
            const TCHAR* PPName;   // FPostProcessSettings 側の名前
            EBlendOp Op;
            double Offset;         // 1 枚増えるごとに足す値 (WhiteTemp の -6500)
        };

        const FGradingField GradingFields[] = {
            {TEXT(""), TEXT("AutoExposureBias"), TEXT("AutoExposureBias"), EBlendOp::Add, 0.0},
            {TEXT(""), TEXT("ColorCorrectionHighlightsMin"), TEXT("ColorCorrectionHighlightsMin"), EBlendOp::Add, 0.0},
            {TEXT(""), TEXT("ColorCorrectionHighlightsMax"), TEXT("ColorCorrectionHighlightsMax"), EBlendOp::Add, 0.0},
            {TEXT(""), TEXT("ColorCorrectionShadowsMax"), TEXT("ColorCorrectionShadowsMax"), EBlendOp::Add, 0.0},

            {TEXT("WhiteBalance"), TEXT("TemperatureType"), TEXT("TemperatureType"), EBlendOp::Override, 0.0},
            {TEXT("WhiteBalance"), TEXT("WhiteTemp"), TEXT("WhiteTemp"), EBlendOp::Add, -6500.0},
            {TEXT("WhiteBalance"), TEXT("WhiteTint"), TEXT("WhiteTint"), EBlendOp::Add, 0.0},

            {TEXT("Global"), TEXT("Saturation"), TEXT("ColorSaturation"), EBlendOp::Multiply, 0.0},
            {TEXT("Global"), TEXT("Contrast"), TEXT("ColorContrast"), EBlendOp::Multiply, 0.0},
            {TEXT("Global"), TEXT("Gamma"), TEXT("ColorGamma"), EBlendOp::Multiply, 0.0},
            {TEXT("Global"), TEXT("Gain"), TEXT("ColorGain"), EBlendOp::Multiply, 0.0},
            {TEXT("Global"), TEXT("Offset"), TEXT("ColorOffset"), EBlendOp::Add, 0.0},

            {TEXT("Shadows"), TEXT("Saturation"), TEXT("ColorSaturationShadows"), EBlendOp::Multiply, 0.0},
            {TEXT("Shadows"), TEXT("Contrast"), TEXT("ColorContrastShadows"), EBlendOp::Multiply, 0.0},
            {TEXT("Shadows"), TEXT("Gamma"), TEXT("ColorGammaShadows"), EBlendOp::Multiply, 0.0},
            {TEXT("Shadows"), TEXT("Gain"), TEXT("ColorGainShadows"), EBlendOp::Multiply, 0.0},
            {TEXT("Shadows"), TEXT("Offset"), TEXT("ColorOffsetShadows"), EBlendOp::Add, 0.0},

            {TEXT("Midtones"), TEXT("Saturation"), TEXT("ColorSaturationMidtones"), EBlendOp::Multiply, 0.0},
            {TEXT("Midtones"), TEXT("Contrast"), TEXT("ColorContrastMidtones"), EBlendOp::Multiply, 0.0},
            {TEXT("Midtones"), TEXT("Gamma"), TEXT("ColorGammaMidtones"), EBlendOp::Multiply, 0.0},
            {TEXT("Midtones"), TEXT("Gain"), TEXT("ColorGainMidtones"), EBlendOp::Multiply, 0.0},
            {TEXT("Midtones"), TEXT("Offset"), TEXT("ColorOffsetMidtones"), EBlendOp::Add, 0.0},

            {TEXT("Highlights"), TEXT("Saturation"), TEXT("ColorSaturationHighlights"), EBlendOp::Multiply, 0.0},
            {TEXT("Highlights"), TEXT("Contrast"), TEXT("ColorContrastHighlights"), EBlendOp::Multiply, 0.0},
            {TEXT("Highlights"), TEXT("Gamma"), TEXT("ColorGammaHighlights"), EBlendOp::Multiply, 0.0},
            {TEXT("Highlights"), TEXT("Gain"), TEXT("ColorGainHighlights"), EBlendOp::Multiply, 0.0},
            {TEXT("Highlights"), TEXT("Offset"), TEXT("ColorOffsetHighlights"), EBlendOp::Add, 0.0},

            {TEXT("Misc"), TEXT("BlueCorrection"), TEXT("BlueCorrection"), EBlendOp::Add, 0.0},
            {TEXT("Misc"), TEXT("ExpandGamut"), TEXT("ExpandGamut"), EBlendOp::Add, 0.0},
            {TEXT("Misc"), TEXT("SceneColorTint"), TEXT("SceneColorTint"), EBlendOp::Add, 0.0},
        };

        /** ImplBlendPostProcessSettings: 複数枚のカラーグレーディングを 1 つの PP にまとめる */
        void BlendGradings(const TArray<FGradingRef>& Gradings, FPostProcessSettings& OutPP)
        {
            const UScriptStruct* PPType = FPostProcessSettings::StaticStruct();

            for (const FGradingField& Field : GradingFields)
            {
                const FProperty* PPProp = FindProp(PPType, Field.PPName);
                if (!PPProp)
                {
                    continue;
                }

                int32 NumOverridden = 0;
                double Acc[4] = {0, 0, 0, 0};
                for (const FGradingRef& Grading : Gradings)
                {
                    // グループ (WhiteBalance 等) の中を見る
                    const UStruct* GroupType = Grading.Type;
                    const void* GroupData = Grading.Data;
                    if (Field.Group[0] != 0)
                    {
                        const UScriptStruct* SubType = nullptr;
                        GroupData = GetStructMember(Grading.Type, Grading.Data, Field.Group, SubType);
                        GroupType = SubType;
                    }
                    if (!GroupType || !GroupData)
                    {
                        continue;
                    }

                    const FBoolProperty* Flag = OverrideFlagFor(GroupType, Field.Name);
                    const FProperty* ValueProp = FindProp(GroupType, Field.Name);
                    if (!Flag || !ValueProp || !Flag->GetPropertyValue_InContainer(GroupData))
                    {
                        continue;
                    }
                    const void* ValuePtr = ValueProp->ContainerPtrToValuePtr<void>(GroupData);

                    if (Field.Op == EBlendOp::Override)
                    {
                        // 後勝ち (同じ型のプロパティなのでそのままコピー)
                        if (ValueProp->SameType(PPProp))
                        {
                            PPProp->CopyCompleteValue(PPProp->ContainerPtrToValuePtr<void>(&OutPP), ValuePtr);
                            SetOverrideFlag(OutPP, Field.PPName);
                        }
                        continue;
                    }

                    double V[4] = {0, 0, 0, 0};
                    const int32 Num = ReadNumeric(ValueProp, ValuePtr, V);
                    if (Num == 0)
                    {
                        continue;
                    }
                    for (int32 Index = 0; Index < 4; ++Index)
                    {
                        if (NumOverridden == 0)
                        {
                            Acc[Index] = V[Index];
                        }
                        else if (Field.Op == EBlendOp::Add)
                        {
                            Acc[Index] += V[Index];
                        }
                        else
                        {
                            Acc[Index] *= V[Index];
                        }
                    }
                    ++NumOverridden;
                }

                if (Field.Op != EBlendOp::Override && NumOverridden > 0)
                {
                    for (int32 Index = 0; Index < 4; ++Index)
                    {
                        Acc[Index] += Field.Offset * (NumOverridden - 1);
                    }
                    WriteNumeric(PPProp, PPProp->ContainerPtrToValuePtr<void>(&OutPP), Acc);
                    SetOverrideFlag(OutPP, Field.PPName);
                }
            }
        }

        /** カスタム PP (Start / Override / Final) を 1 枚適用 */
        bool ApplyCustomLayer(UObject* Viewport, const TCHAR* LayerName, FPostProcessSettings& InOutPP, FString& Report)
        {
            if (!Viewport)
            {
                return false;
            }
            const UScriptStruct* RenderType = nullptr;
            const void* Render = GetStructMember(Viewport->GetClass(), Viewport, TEXT("RenderSettings"), RenderType);
            const UScriptStruct* CustomType = nullptr;
            const void* Custom = GetStructMember(RenderType, Render, TEXT("CustomPostprocess"), CustomType);
            const UScriptStruct* LayerType = nullptr;
            const void* Layer = GetStructMember(CustomType, Custom, LayerName, LayerType);
            if (!Layer || !GetBool(LayerType, Layer, TEXT("bIsEnabled")))
            {
                return false;
            }

            const UScriptStruct* PPType = nullptr;
            const void* PP = GetStructMember(LayerType, Layer, TEXT("PostProcessSettings"), PPType);
            if (!PP || PPType != FPostProcessSettings::StaticStruct())
            {
                return false;
            }

            const float Weight = GetFloat(LayerType, Layer, TEXT("BlendWeight"), 1.0f);
            OverridePostProcess(InOutPP, *static_cast<const FPostProcessSettings*>(PP), Weight);
            Report += FString::Printf(TEXT("カスタム PP %s (重み %.2f)\n"), LayerName, Weight);
            return true;
        }
    }

    TArray<FString> GetViewportIds(const AActor* RootActor)
    {
        TArray<FString> Ids;
        ForEachViewport(GetConfigData(RootActor), [&Ids](const FString& Id, UObject*) { Ids.Add(Id); });
        return Ids;
    }

    bool BuildOuterViewportPostProcess(const AActor* RootActor, const FString& ViewportId, const FPostProcessSettings& ScenePP,
                                       FPostProcessSettings& OutPP, FString& OutReport)
    {
        OutReport.Reset();
        OutPP = ScenePP;

        UObject* ConfigData = GetConfigData(RootActor);
        if (!ConfigData)
        {
            OutReport = TEXT("nDisplay の RootActor (設定データ) が見つかりません");
            return false;
        }

        UObject* Viewport = nullptr;
        if (!ViewportId.IsEmpty())
        {
            ForEachViewport(ConfigData, [&](const FString& Id, UObject* Vp)
            {
                if (!Viewport && Id.Equals(ViewportId, ESearchCase::IgnoreCase))
                {
                    Viewport = Vp;
                }
            });
            if (!Viewport)
            {
                OutReport += FString::Printf(TEXT("ビューポート '%s' が見つかりません (クラスタ全体のグレーディングのみ適用)\n"), *ViewportId);
            }
        }

        // Start (シーンより下) → シーン → Override → Final
        FPostProcessSettings Composite;
        ApplyCustomLayer(Viewport, TEXT("Start"), Composite, OutReport);
        OverridePostProcess(Composite, ScenePP, 1.0f);
        ApplyCustomLayer(Viewport, TEXT("Override"), Composite, OutReport);
        ApplyCustomLayer(Viewport, TEXT("Final"), Composite, OutReport);

        // FinalPerViewport: ステージのカラーグレーディング
        const UScriptStruct* StageType = nullptr;
        const void* Stage = GetStructMember(ConfigData->GetClass(), ConfigData, TEXT("StageSettings"), StageType);
        if (Stage && GetBool(StageType, Stage, TEXT("EnableColorGrading"), true))
        {
            TArray<FGradingRef> Gradings;
            float Weight = 1.0f;

            // Per-Viewport (有効でこのビューポートを対象にしている最初のもの)
            const void* PerViewport = nullptr;
            const UScriptStruct* PerViewportType = nullptr;
            if (const FArrayProperty* PerViewportArray = CastField<FArrayProperty>(FindProp(StageType, TEXT("PerViewportColorGrading"))))
            {
                const FStructProperty* Inner = CastField<FStructProperty>(PerViewportArray->Inner);
                FScriptArrayHelper Array(PerViewportArray, PerViewportArray->ContainerPtrToValuePtr<void>(Stage));
                for (int32 Index = 0; Inner && Index < Array.Num() && !ViewportId.IsEmpty(); ++Index)
                {
                    const void* Item = Array.GetRawPtr(Index);
                    if (!GetBool(Inner->Struct, Item, TEXT("bIsEnabled"), true))
                    {
                        continue;
                    }
                    const FArrayProperty* Targets = CastField<FArrayProperty>(FindProp(Inner->Struct, TEXT("ApplyPostProcessToObjects")));
                    if (!Targets)
                    {
                        continue;
                    }
                    FScriptArrayHelper TargetArray(Targets, Targets->ContainerPtrToValuePtr<void>(Item));
                    for (int32 T = 0; T < TargetArray.Num(); ++T)
                    {
                        if (static_cast<const FString*>(static_cast<const void*>(TargetArray.GetRawPtr(T)))->Equals(ViewportId, ESearchCase::IgnoreCase))
                        {
                            PerViewport = Item;
                            PerViewportType = Inner->Struct;
                            break;
                        }
                    }
                    if (PerViewport)
                    {
                        break;
                    }
                }
            }

            // Entire Cluster (全体が有効で、Per-Viewport が「クラスタ全体を含める」場合)
            const UScriptStruct* ClusterType = nullptr;
            const void* Cluster = GetStructMember(StageType, Stage, TEXT("EntireClusterColorGrading"), ClusterType);
            const bool bUseCluster = Cluster && GetBool(ClusterType, Cluster, TEXT("bEnableEntireClusterColorGrading"), true) &&
                                     (!PerViewport || GetBool(PerViewportType, PerViewport, TEXT("bIsEntireClusterEnabled"), true));

            auto AddGrading = [&](const UScriptStruct* OwnerType, const void* Owner, const TCHAR* Label)
            {
                const UScriptStruct* GradingType = nullptr;
                const void* Grading = GetStructMember(OwnerType, Owner, TEXT("ColorGradingSettings"), GradingType);
                if (!Grading)
                {
                    return;
                }
                const float W = GetFloat(GradingType, Grading, TEXT("BlendWeight"), 1.0f);
                if (W <= 0.0f)
                {
                    return;
                }
                Gradings.Add({GradingType, Grading});
                Weight *= W;
                OutReport += FString::Printf(TEXT("カラーグレーディング: %s\n"), Label);
            };

            if (bUseCluster)
            {
                AddGrading(ClusterType, Cluster, TEXT("Entire Cluster"));
            }
            if (PerViewport)
            {
                AddGrading(PerViewportType, PerViewport, *FString::Printf(TEXT("Per-Viewport (%s)"), *ViewportId));
            }

            if (Gradings.Num() > 0)
            {
                FPostProcessSettings Graded;
                BlendGradings(Gradings, Graded);
                OverridePostProcess(Composite, Graded, Weight);
            }
        }
        else
        {
            OutReport += TEXT("ステージのカラーグレーディングは無効\n");
        }

        OutPP = Composite;
        return true;
    }

    // ============================================================
    // OCIO
    // ============================================================

    int32 SelectViewportOCIO(bool bAllViewportsEnabled, TConstArrayView<FOCIOProfileDesc> Profiles, const FString& ViewportId)
    {
        if (!bAllViewportsEnabled)
        {
            return -2;
        }
        for (int32 Index = 0; Index < Profiles.Num(); ++Index)
        {
            if (!Profiles[Index].bIsEnabled)
            {
                continue;
            }
            for (const FString& Object : Profiles[Index].ApplyToObjects)
            {
                if (ViewportId.Compare(Object, ESearchCase::IgnoreCase) == 0)
                {
                    return Index;
                }
            }
        }
        return -1;
    }

    bool FindOuterViewportOCIO(const AActor* RootActor, const FString& ViewportId,
                               FOpenColorIOColorConversionSettings& OutSettings, FString& OutReport)
    {
        OutReport.Reset();
        UObject* ConfigData = GetConfigData(RootActor);
        if (!ConfigData)
        {
            OutReport = TEXT("nDisplay の RootActor (設定データ) が見つかりません");
            return false;
        }

        const UScriptStruct* StageType = nullptr;
        const void* Stage = GetStructMember(ConfigData->GetClass(), ConfigData, TEXT("StageSettings"), StageType);
        const UScriptStruct* OCIOType = nullptr;
        const void* ViewportOCIO = GetStructMember(StageType, Stage, TEXT("ViewportOCIO"), OCIOType);
        const UScriptStruct* AllType = nullptr;
        const void* All = GetStructMember(OCIOType, ViewportOCIO, TEXT("AllViewportsOCIOConfiguration"), AllType);
        if (!All)
        {
            OutReport = TEXT("この nDisplay には OCIO の設定がありません");
            return false;
        }

        // プロファイルを並べて、nDisplay と同じ規則で選ぶ
        TArray<FOCIOProfileDesc> Profiles;
        TArray<const void*> ProfileData;
        const UScriptStruct* ProfileType = nullptr;
        if (const FArrayProperty* ProfileArray = CastField<FArrayProperty>(FindProp(OCIOType, TEXT("PerViewportOCIOProfiles"))))
        {
            const FStructProperty* Inner = CastField<FStructProperty>(ProfileArray->Inner);
            ProfileType = Inner ? Inner->Struct : nullptr;
            FScriptArrayHelper Array(ProfileArray, ProfileArray->ContainerPtrToValuePtr<void>(ViewportOCIO));
            for (int32 Index = 0; ProfileType && Index < Array.Num(); ++Index)
            {
                const void* Item = Array.GetRawPtr(Index);
                FOCIOProfileDesc& Desc = Profiles.AddDefaulted_GetRef();
                Desc.bIsEnabled = GetBool(ProfileType, Item, TEXT("bIsEnabled"));
                if (const FArrayProperty* Targets = CastField<FArrayProperty>(FindProp(ProfileType, TEXT("ApplyOCIOToObjects"))))
                {
                    FScriptArrayHelper TargetArray(Targets, Targets->ContainerPtrToValuePtr<void>(Item));
                    for (int32 T = 0; T < TargetArray.Num(); ++T)
                    {
                        Desc.ApplyToObjects.Add(*static_cast<const FString*>(static_cast<const void*>(TargetArray.GetRawPtr(T))));
                    }
                }
                ProfileData.Add(Item);
            }
        }

        const int32 Selected = SelectViewportOCIO(GetBool(AllType, All, TEXT("bIsEnabled")), Profiles, ViewportId);
        if (Selected == -2)
        {
            OutReport = TEXT("nDisplay の OCIO (All Viewports) は無効");
            return false;
        }

        const UScriptStruct* ColorType = nullptr;
        const void* Color = Selected >= 0
            ? GetStructMember(ProfileType, ProfileData[Selected], TEXT("ColorConfiguration"), ColorType)
            : GetStructMember(AllType, All, TEXT("ColorConfiguration"), ColorType);
        if (!Color || ColorType != FOpenColorIOColorConversionSettings::StaticStruct())
        {
            OutReport = TEXT("nDisplay の OCIO の設定を読めませんでした");
            return false;
        }

        OutSettings = *static_cast<const FOpenColorIOColorConversionSettings*>(Color);
        const FString Which = Selected >= 0 ? FString::Printf(TEXT("ビューポート '%s' のプロファイル"), *ViewportId) : FString(TEXT("全ビューポート"));
        if (!LightSyncOCIO::IsConfigured(OutSettings))
        {
            OutReport = FString::Printf(TEXT("nDisplay の OCIO (%s) は変換が未設定のため掛けない"), *Which);
            return false;
        }
        OutReport = FString::Printf(TEXT("nDisplay の OCIO (%s): %s"), *Which, *LightSyncOCIO::Describe(OutSettings));
        return true;
    }
}
