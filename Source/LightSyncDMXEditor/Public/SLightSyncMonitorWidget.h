// Copyright UE-Comp. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "UObject/StrongObjectPtr.h"
#include "LightSyncCalibrator.h"

class ULightSyncSubsystem;
class ALightProbeActor;

class STextBlock;

/**
 * SLightSyncMonitorWidget
 *
 * エディタ上で全LightProbeのステータスをリアルタイム表示し、
 * プローブ選択でDMX/OSC設定を直接編集できる統合ウィンドウ。
 *
 * 上段: プローブ一覧 (クリック選択)
 * 下段: 選択プローブの設定 (基本/色の基準とゾーン/DMX/OSC)
 */
class SLightSyncMonitorWidget : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SLightSyncMonitorWidget) {}
    SLATE_END_ARGS()

    virtual ~SLightSyncMonitorWidget();

    void Construct(const FArguments& InArgs);
    virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
    // === UI構築 ===
    TSharedRef<SWidget> BuildToolbar();
    TSharedRef<SWidget> BuildProbeList();

    // === リスト/設定パネル管理 ===
    void RefreshProbeList();
    void RefreshSettingsPanel();
    void SelectProbe(const FString& ProbeName);

    // === 設定セクション構築 ===
    void BuildGeneralSection(ALightProbeActor* Probe);
    void BuildReferenceSection(ALightProbeActor* Probe);
    void BuildDMXSection(ALightProbeActor* Probe);
    void BuildOSCSection(ALightProbeActor* Probe);
    void BuildCalibrationSection(ALightProbeActor* Probe);
    TSharedRef<SWidget> MakeStepRow(bool bWall, int32 Index);

    // === レイアウトヘルパー ===
    void AddSectionHeader(const FText& Title);
    void AddLabeledRow(const FText& Label, TSharedRef<SWidget> ValueWidget);

    // === ボタンコールバック ===
    FReply OnActivateAllClicked();
    FReply OnDeactivateAllClicked();
    FReply OnBlackoutClicked();
    FReply OnRefreshClicked();
    FReply OnForceSampleClicked();

    // === ユーティリティ ===
    ULightSyncSubsystem* GetSubsystem() const;
    ALightProbeActor* FindProbeByName(const FString& Name) const;
    TArray<ALightProbeActor*> GetAllProbeActors() const;

    // === 状態 ===
    float RefreshTimer = 0.0f;
    float RefreshInterval = 0.5f;
    int32 CachedProbeCount = -1;

    TSharedPtr<SVerticalBox> ProbeListBox;
    TSharedPtr<SVerticalBox> SettingsPanelBox;

    float MasterDimmerValue = 1.0f;
    FString SelectedProbeName;

    /** カメラキャリブレーション (選択プローブ用) */
    TStrongObjectPtr<ULightSyncCalibrator> Calibrator;
    TSharedPtr<STextBlock> CalibrationReport;
};
