// Copyright UE-Comp. All Rights Reserved.

#include "SLightSyncMonitorWidget.h"
#include "LightSyncSubsystem.h"
#include "LightProbeActor.h"
#include "DMXColorOutputComponent.h"
#include "OSCColorOutputComponent.h"

#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Styling/AppStyle.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"

#define LOCTEXT_NAMESPACE "SLightSyncMonitor"

// ============================================================
// カラーモード / OSCフォーマット名 (表示用)
// ============================================================

static const TCHAR* ColorModeNames[] = {
    TEXT("RGB (3ch)"),
    TEXT("RGBW (4ch)"),
    TEXT("RGBAW (5ch)"),
    TEXT("Dimmer+RGB (4ch)"),
    TEXT("CCT+Brightness (2ch)")
};
static constexpr int32 NumColorModes = UE_ARRAY_COUNT(ColorModeNames);

static const TCHAR* OSCFormatNames[] = {
    TEXT("Float RGB"),
    TEXT("Int RGB (0-255)"),
    TEXT("Float RGBA"),
    TEXT("QLC+ 互換"),
    TEXT("DasLight 互換"),
    TEXT("カスタム")
};
static constexpr int32 NumOSCFormats = UE_ARRAY_COUNT(OSCFormatNames);

// ============================================================
// Construct
// ============================================================

void SLightSyncMonitorWidget::Construct(const FArguments& InArgs)
{
    SAssignNew(SettingsPanelBox, SVerticalBox);

    ChildSlot
    [
        SNew(SVerticalBox)

        // === タイトル ===
        + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(4.0f)
            [
                SNew(STextBlock)
                    .Text(LOCTEXT("Title", "LightSync DMX Monitor"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
            ]

        + SVerticalBox::Slot().AutoHeight()[SNew(SSeparator)]

        // === ツールバー ===
        + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(4.0f)
            [BuildToolbar()]

        + SVerticalBox::Slot().AutoHeight()[SNew(SSeparator)]

        // === メイン: 上=プローブ一覧 / 下=設定パネル ===
        + SVerticalBox::Slot()
            .FillHeight(1.0f)
            [
                SNew(SSplitter)
                    .Orientation(Orient_Vertical)

                + SSplitter::Slot()
                    .Value(0.35f)
                    [
                        SNew(SScrollBox)
                        + SScrollBox::Slot()
                            [BuildProbeList()]
                    ]

                + SSplitter::Slot()
                    .Value(0.65f)
                    [
                        SNew(SScrollBox)
                        + SScrollBox::Slot()
                            [SettingsPanelBox.ToSharedRef()]
                    ]
            ]
    ];

    // 初期メッセージ
    RefreshSettingsPanel();
}

// ============================================================
// Tick
// ============================================================

void SLightSyncMonitorWidget::Tick(
    const FGeometry& AllottedGeometry,
    const double InCurrentTime,
    const float InDeltaTime)
{
    SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

    RefreshTimer += InDeltaTime;
    if (RefreshTimer >= RefreshInterval)
    {
        RefreshTimer = 0.0f;
        RefreshProbeList();
    }
}

// ============================================================
// ツールバー
// ============================================================

TSharedRef<SWidget> SLightSyncMonitorWidget::BuildToolbar()
{
    return SNew(SHorizontalBox)

        + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
            [SNew(SButton)
                .Text(LOCTEXT("ActivateAll", "全て有効"))
                .ToolTipText(LOCTEXT("ActivateAllTip", "全プローブを有効にする"))
                .OnClicked(this, &SLightSyncMonitorWidget::OnActivateAllClicked)]

        + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
            [SNew(SButton)
                .Text(LOCTEXT("DeactivateAll", "全て無効"))
                .ToolTipText(LOCTEXT("DeactivateAllTip", "全プローブを無効にする"))
                .OnClicked(this, &SLightSyncMonitorWidget::OnDeactivateAllClicked)]

        + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
            [SNew(SButton)
                .Text(LOCTEXT("Blackout", "ブラックアウト"))
                .ToolTipText(LOCTEXT("BlackoutTip", "全DMX出力を0にする"))
                .OnClicked(this, &SLightSyncMonitorWidget::OnBlackoutClicked)]

        + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
            [SNew(SButton)
                .Text(LOCTEXT("ForceSample", "手動サンプル"))
                .ToolTipText(LOCTEXT("ForceSampleTip", "全プローブを今すぐサンプリング"))
                .OnClicked(this, &SLightSyncMonitorWidget::OnForceSampleClicked)]

        + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
            [SNew(SButton)
                .Text(LOCTEXT("Refresh", "更新"))
                .OnClicked(this, &SLightSyncMonitorWidget::OnRefreshClicked)]

        + SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 2.0f).VAlign(VAlign_Center)
            [SNew(STextBlock).Text(LOCTEXT("MasterDimmer", "マスターディマー:"))]

        + SHorizontalBox::Slot().AutoWidth().Padding(2.0f)
            [SNew(SBox).WidthOverride(100.0f)
                [SNew(SSpinBox<float>)
                    .MinValue(0.0f).MaxValue(1.0f).Delta(0.01f)
                    .Value_Lambda([this]() { return MasterDimmerValue; })
                    .OnValueChanged_Lambda([this](float NewValue) {
                        MasterDimmerValue = NewValue;
                        if (ULightSyncSubsystem* Sub = GetSubsystem())
                        {
                            Sub->SetMasterDimmer(NewValue);
                        }
                    })]];
}

// ============================================================
// プローブ一覧
// ============================================================

TSharedRef<SWidget> SLightSyncMonitorWidget::BuildProbeList()
{
    SAssignNew(ProbeListBox, SVerticalBox);
    RefreshProbeList();
    return ProbeListBox.ToSharedRef();
}

void SLightSyncMonitorWidget::RefreshProbeList()
{
    if (!ProbeListBox.IsValid())
    {
        return;
    }

    ProbeListBox->ClearChildren();

    TArray<ALightProbeActor*> Probes = GetAllProbeActors();

    if (Probes.Num() == 0)
    {
        FText Message = GetSubsystem()
            ? LOCTEXT("NoProbes", "プローブが見つかりません。LightProbeActorをシーンに配置してください。")
            : LOCTEXT("NoWorld", "ワールドが見つかりません。");

        ProbeListBox->AddSlot().AutoHeight().Padding(8.0f)
            [SNew(STextBlock).Text(Message)
                .ColorAndOpacity(FSlateColor(FLinearColor::Yellow))];
        return;
    }

    // --- ヘッダー行 ---
    ProbeListBox->AddSlot().AutoHeight().Padding(2.0f)
        [SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(0.20f).Padding(2.0f)
                [SNew(STextBlock).Text(LOCTEXT("ColName", "名前"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))]
            + SHorizontalBox::Slot().FillWidth(0.06f).Padding(2.0f)
                [SNew(STextBlock).Text(LOCTEXT("ColSt", "状態"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))]
            + SHorizontalBox::Slot().FillWidth(0.06f).Padding(2.0f)
                [SNew(STextBlock).Text(LOCTEXT("ColClr", "色"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))]
            + SHorizontalBox::Slot().FillWidth(0.20f).Padding(2.0f)
                [SNew(STextBlock).Text(LOCTEXT("ColRGB", "RGB"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))]
            + SHorizontalBox::Slot().FillWidth(0.14f).Padding(2.0f)
                [SNew(STextBlock).Text(LOCTEXT("ColDMX", "DMX"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))]
            + SHorizontalBox::Slot().FillWidth(0.06f).Padding(2.0f)
                [SNew(STextBlock).Text(LOCTEXT("ColOSC", "OSC"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))]
            + SHorizontalBox::Slot().FillWidth(0.28f).Padding(2.0f)
                [SNew(STextBlock).Text(LOCTEXT("ColLoc", "位置"))
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))]];

    ProbeListBox->AddSlot().AutoHeight()[SNew(SSeparator)];

    // --- 各プローブの行 ---
    for (ALightProbeActor* Probe : Probes)
    {
        if (!Probe)
        {
            continue;
        }

        const FString ProbeName = Probe->GetProbeName();
        const FLinearColor Color = Probe->GetCurrentColor();
        const bool bIsSelected = (ProbeName == SelectedProbeName);
        const FLinearColor RowBg = bIsSelected
            ? FLinearColor(0.12f, 0.22f, 0.42f)
            : FLinearColor(0.03f, 0.03f, 0.03f);

        const FString RGBText = FString::Printf(TEXT("R:%3d G:%3d B:%3d"),
            FMath::RoundToInt(Color.R * 255),
            FMath::RoundToInt(Color.G * 255),
            FMath::RoundToInt(Color.B * 255));

        // DMX情報
        FString DMXInfo = TEXT("-");
        if (Probe->bUseDMXOutput && Probe->DMXOutput
            && Probe->DMXOutput->FixtureMappings.Num() > 0)
        {
            const FDMXFixtureMapping& M = Probe->DMXOutput->FixtureMappings[0];
            DMXInfo = FString::Printf(TEXT("U%d Ch%d"), M.Universe, M.StartChannel);
        }

        // OSC情報
        const FString OSCInfo = Probe->bUseOSCOutput ? TEXT("ON") : TEXT("-");

        // 位置
        const FVector Loc = Probe->GetActorLocation();
        const FString LocText = FString::Printf(TEXT("%.0f, %.0f, %.0f"), Loc.X, Loc.Y, Loc.Z);

        ProbeListBox->AddSlot().AutoHeight()
            [
                SNew(SBorder)
                    .BorderBackgroundColor(FSlateColor(RowBg))
                    .Padding(FMargin(0.0f))
                    [
                        SNew(SButton)
                            .OnClicked_Lambda([this, ProbeName]() -> FReply {
                                SelectProbe(ProbeName);
                                return FReply::Handled();
                            })
                            [
                                SNew(SHorizontalBox)

                                // 名前
                                + SHorizontalBox::Slot().FillWidth(0.20f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(STextBlock).Text(FText::FromString(ProbeName))]

                                // 状態
                                + SHorizontalBox::Slot().FillWidth(0.06f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(STextBlock)
                                        .Text(Probe->bIsProbeActive
                                            ? LOCTEXT("Active", "●")
                                            : LOCTEXT("Inactive", "○"))
                                        .ColorAndOpacity(Probe->bIsProbeActive
                                            ? FSlateColor(FLinearColor::Green)
                                            : FSlateColor(FLinearColor::Gray))]

                                // 色プレビュー
                                + SHorizontalBox::Slot().FillWidth(0.06f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(SBox).WidthOverride(32.0f).HeightOverride(16.0f)
                                        [SNew(SColorBlock).Color(Color)
                                            .ShowBackgroundForAlpha(false)]]

                                // RGB
                                + SHorizontalBox::Slot().FillWidth(0.20f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(STextBlock).Text(FText::FromString(RGBText))]

                                // DMX
                                + SHorizontalBox::Slot().FillWidth(0.14f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(STextBlock).Text(FText::FromString(DMXInfo))]

                                // OSC
                                + SHorizontalBox::Slot().FillWidth(0.06f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(STextBlock).Text(FText::FromString(OSCInfo))]

                                // 位置
                                + SHorizontalBox::Slot().FillWidth(0.28f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(STextBlock).Text(FText::FromString(LocText))]
                            ]
                    ]
            ];
    }
}

// ============================================================
// プローブ選択 → 設定パネル更新
// ============================================================

void SLightSyncMonitorWidget::SelectProbe(const FString& ProbeName)
{
    SelectedProbeName = ProbeName;
    RefreshProbeList();
    RefreshSettingsPanel();
}

void SLightSyncMonitorWidget::RefreshSettingsPanel()
{
    if (!SettingsPanelBox.IsValid())
    {
        return;
    }

    SettingsPanelBox->ClearChildren();

    ALightProbeActor* Probe = FindProbeByName(SelectedProbeName);
    if (!Probe)
    {
        SettingsPanelBox->AddSlot().AutoHeight().Padding(8.0f)
            [SNew(STextBlock)
                .Text(LOCTEXT("SelectProbe", "▲ 上のリストからプローブを選択すると、ここに設定が表示されます。"))
                .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))];
        return;
    }

    // ヘッダー
    SettingsPanelBox->AddSlot().AutoHeight().Padding(4.0f)
        [SNew(STextBlock)
            .Text(FText::Format(LOCTEXT("SelTitle", "選択プローブ: {0}"),
                FText::FromString(SelectedProbeName)))
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))];
    SettingsPanelBox->AddSlot().AutoHeight()[SNew(SSeparator)];

    BuildGeneralSection(Probe);
    BuildColorCorrectionSection(Probe);
    BuildDMXSection(Probe);
    BuildOSCSection(Probe);
}

// ============================================================
// 基本設定セクション
// ============================================================

void SLightSyncMonitorWidget::BuildGeneralSection(ALightProbeActor* Probe)
{
    TWeakObjectPtr<ALightProbeActor> W(Probe);

    AddSectionHeader(LOCTEXT("SecGeneral", "基本設定"));

    // プローブ名
    AddLabeledRow(LOCTEXT("ProbeName", "プローブ名"),
        SNew(SEditableTextBox)
            .Text_Lambda([W]() -> FText {
                return W.IsValid() ? FText::FromString(W->ProbeName) : FText::GetEmpty();
            })
            .OnTextCommitted_Lambda([W](const FText& T, ETextCommit::Type) {
                if (W.IsValid()) W->ProbeName = T.ToString();
            }));

    // 有効/無効
    AddLabeledRow(LOCTEXT("ProbeActive", "有効"),
        SNew(SCheckBox)
            .IsChecked_Lambda([W]() -> ECheckBoxState {
                return (W.IsValid() && W->bIsProbeActive)
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([W](ECheckBoxState S) {
                if (W.IsValid()) W->SetProbeActive(S == ECheckBoxState::Checked);
            }));

    // サンプリングレート
    AddLabeledRow(LOCTEXT("SampRate", "サンプリングレート (Hz)"),
        SNew(SSpinBox<float>)
            .MinValue(1.0f).MaxValue(120.0f).Delta(1.0f)
            .Value_Lambda([W]() { return W.IsValid() ? W->SamplingRate : 30.0f; })
            .OnValueChanged_Lambda([W](float V) { if (W.IsValid()) W->SamplingRate = V; }));

    // キャプチャ解像度
    AddLabeledRow(LOCTEXT("CaptureRes", "キャプチャ解像度"),
        SNew(SSpinBox<int32>)
            .MinValue(16).MaxValue(512).Delta(16)
            .Value_Lambda([W]() { return W.IsValid() ? W->CaptureResolution : 64; })
            .OnValueChanged_Lambda([W](int32 V) { if (W.IsValid()) W->CaptureResolution = V; }));

    // DMX出力トグル
    AddLabeledRow(LOCTEXT("UseDMX", "DMX出力を使用"),
        SNew(SCheckBox)
            .IsChecked_Lambda([W]() -> ECheckBoxState {
                return (W.IsValid() && W->bUseDMXOutput)
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([W](ECheckBoxState S) {
                if (W.IsValid()) W->bUseDMXOutput = (S == ECheckBoxState::Checked);
            }));

    // OSC出力トグル
    AddLabeledRow(LOCTEXT("UseOSC", "OSC出力を使用"),
        SNew(SCheckBox)
            .IsChecked_Lambda([W]() -> ECheckBoxState {
                return (W.IsValid() && W->bUseOSCOutput)
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([W](ECheckBoxState S) {
                if (W.IsValid()) W->bUseOSCOutput = (S == ECheckBoxState::Checked);
            }));
}

// ============================================================
// 色補正セクション
// ============================================================

void SLightSyncMonitorWidget::BuildColorCorrectionSection(ALightProbeActor* Probe)
{
    TWeakObjectPtr<ALightProbeActor> W(Probe);

    AddSectionHeader(LOCTEXT("SecColor", "色補正"));

    AddLabeledRow(LOCTEXT("Gamma", "ガンマ"),
        SNew(SSpinBox<float>)
            .MinValue(0.1f).MaxValue(5.0f).Delta(0.1f)
            .Value_Lambda([W]() { return W.IsValid() ? W->GammaCorrection : 2.2f; })
            .OnValueChanged_Lambda([W](float V) { if (W.IsValid()) W->GammaCorrection = V; }));

    AddLabeledRow(LOCTEXT("ColorTemp", "色温度オフセット (K)"),
        SNew(SSpinBox<float>)
            .MinValue(-3000.0f).MaxValue(3000.0f).Delta(100.0f)
            .Value_Lambda([W]() { return W.IsValid() ? W->ColorTemperatureOffset : 0.0f; })
            .OnValueChanged_Lambda([W](float V) { if (W.IsValid()) W->ColorTemperatureOffset = V; }));

    AddLabeledRow(LOCTEXT("Saturation", "彩度"),
        SNew(SSpinBox<float>)
            .MinValue(0.0f).MaxValue(3.0f).Delta(0.05f)
            .Value_Lambda([W]() { return W.IsValid() ? W->SaturationMultiplier : 1.0f; })
            .OnValueChanged_Lambda([W](float V) { if (W.IsValid()) W->SaturationMultiplier = V; }));

    AddLabeledRow(LOCTEXT("Brightness", "明度"),
        SNew(SSpinBox<float>)
            .MinValue(0.0f).MaxValue(3.0f).Delta(0.05f)
            .Value_Lambda([W]() { return W.IsValid() ? W->BrightnessMultiplier : 1.0f; })
            .OnValueChanged_Lambda([W](float V) { if (W.IsValid()) W->BrightnessMultiplier = V; }));

    // 色のみモード
    AddLabeledRow(LOCTEXT("ColorOnly", "色のみモード (明度は照明側で制御)"),
        SNew(SCheckBox)
            .IsChecked_Lambda([W]() -> ECheckBoxState {
                return (W.IsValid() && W->bColorOnly)
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([W](ECheckBoxState S) {
                if (W.IsValid()) W->bColorOnly = (S == ECheckBoxState::Checked);
            }));

    AddLabeledRow(LOCTEXT("BlackThresh", "ブラック閾値 (色のみモード)"),
        SNew(SSpinBox<float>)
            .MinValue(0.0f).MaxValue(0.1f).Delta(0.005f)
            .Value_Lambda([W]() { return W.IsValid() ? W->ColorOnlyBlackThreshold : 0.01f; })
            .OnValueChanged_Lambda([W](float V) { if (W.IsValid()) W->ColorOnlyBlackThreshold = V; }));
}

// ============================================================
// DMX出力設定セクション
// ============================================================

void SLightSyncMonitorWidget::BuildDMXSection(ALightProbeActor* Probe)
{
    TWeakObjectPtr<ALightProbeActor> W(Probe);

    AddSectionHeader(LOCTEXT("SecDMX", "DMX出力設定"));

    if (!Probe->DMXOutput || Probe->DMXOutput->FixtureMappings.Num() == 0)
    {
        SettingsPanelBox->AddSlot().AutoHeight().Padding(8.0f, 2.0f)
            [SNew(STextBlock)
                .Text(LOCTEXT("NoDMX", "フィクスチャマッピングがありません"))
                .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))];
        return;
    }

    // DMX出力有効
    AddLabeledRow(LOCTEXT("DMXEnabled", "DMX有効"),
        SNew(SCheckBox)
            .IsChecked_Lambda([W]() -> ECheckBoxState {
                return (W.IsValid() && W->DMXOutput && W->DMXOutput->bDMXOutputEnabled)
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([W](ECheckBoxState S) {
                if (W.IsValid() && W->DMXOutput)
                    W->DMXOutput->bDMXOutputEnabled = (S == ECheckBoxState::Checked);
            }));

    // フィクスチャ名
    AddLabeledRow(LOCTEXT("FixtureName", "フィクスチャ名"),
        SNew(SEditableTextBox)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    return FText::FromString(W->DMXOutput->FixtureMappings[0].FixtureName);
                return FText::GetEmpty();
            })
            .OnTextCommitted_Lambda([W](const FText& T, ETextCommit::Type) {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    W->DMXOutput->FixtureMappings[0].FixtureName = T.ToString();
            }));

    // Universe
    AddLabeledRow(LOCTEXT("Universe", "Universe"),
        SNew(SSpinBox<int32>)
            .MinValue(1).MaxValue(64000).Delta(1)
            .Value_Lambda([W]() -> int32 {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    return W->DMXOutput->FixtureMappings[0].Universe;
                return 1;
            })
            .OnValueChanged_Lambda([W](int32 V) {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    W->DMXOutput->FixtureMappings[0].Universe = V;
            }));

    // 開始チャンネル
    AddLabeledRow(LOCTEXT("StartCh", "開始チャンネル"),
        SNew(SSpinBox<int32>)
            .MinValue(1).MaxValue(512).Delta(1)
            .Value_Lambda([W]() -> int32 {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    return W->DMXOutput->FixtureMappings[0].StartChannel;
                return 1;
            })
            .OnValueChanged_Lambda([W](int32 V) {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    W->DMXOutput->FixtureMappings[0].StartChannel = V;
            }));

    // カラーモード (スピンボックス + ラベル)
    AddLabeledRow(LOCTEXT("ColorMode", "カラーモード"),
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(0.4f)
            [SNew(SSpinBox<int32>)
                .MinValue(0).MaxValue(NumColorModes - 1).Delta(1)
                .Value_Lambda([W]() -> int32 {
                    if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                        return static_cast<int32>(W->DMXOutput->FixtureMappings[0].ColorMode);
                    return 0;
                })
                .OnValueChanged_Lambda([W](int32 V) {
                    if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                        W->DMXOutput->FixtureMappings[0].ColorMode = static_cast<ELightSyncColorMode>(V);
                })]
        + SHorizontalBox::Slot().FillWidth(0.6f).VAlign(VAlign_Center).Padding(6.0f, 0.0f)
            [SNew(STextBlock)
                .Text_Lambda([W]() -> FText {
                    if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    {
                        int32 Idx = FMath::Clamp(
                            static_cast<int32>(W->DMXOutput->FixtureMappings[0].ColorMode),
                            0, NumColorModes - 1);
                        return FText::FromString(ColorModeNames[Idx]);
                    }
                    return FText::GetEmpty();
                })]);

    // 明度スケール
    AddLabeledRow(LOCTEXT("BrightScale", "明度スケール"),
        SNew(SSpinBox<float>)
            .MinValue(0.0f).MaxValue(2.0f).Delta(0.05f)
            .Value_Lambda([W]() -> float {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    return W->DMXOutput->FixtureMappings[0].BrightnessScale;
                return 1.0f;
            })
            .OnValueChanged_Lambda([W](float V) {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    W->DMXOutput->FixtureMappings[0].BrightnessScale = V;
            }));

    // マスターディマー (個別)
    AddLabeledRow(LOCTEXT("DMXDimmer", "ディマー"),
        SNew(SSpinBox<float>)
            .MinValue(0.0f).MaxValue(1.0f).Delta(0.01f)
            .Value_Lambda([W]() -> float {
                return (W.IsValid() && W->DMXOutput) ? W->DMXOutput->MasterDimmer : 1.0f;
            })
            .OnValueChanged_Lambda([W](float V) {
                if (W.IsValid() && W->DMXOutput) W->DMXOutput->MasterDimmer = V;
            }));
}

// ============================================================
// OSC出力設定セクション
// ============================================================

void SLightSyncMonitorWidget::BuildOSCSection(ALightProbeActor* Probe)
{
    TWeakObjectPtr<ALightProbeActor> W(Probe);

    AddSectionHeader(LOCTEXT("SecOSC", "OSC出力設定"));

    if (!Probe->OSCOutput)
    {
        SettingsPanelBox->AddSlot().AutoHeight().Padding(8.0f, 2.0f)
            [SNew(STextBlock)
                .Text(LOCTEXT("NoOSC", "OSCコンポーネントがありません"))
                .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))];
        return;
    }

    // OSC有効
    AddLabeledRow(LOCTEXT("OSCEnabled", "OSC有効"),
        SNew(SCheckBox)
            .IsChecked_Lambda([W]() -> ECheckBoxState {
                return (W.IsValid() && W->OSCOutput && W->OSCOutput->bOSCOutputEnabled)
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([W](ECheckBoxState S) {
                if (W.IsValid() && W->OSCOutput)
                    W->OSCOutput->bOSCOutputEnabled = (S == ECheckBoxState::Checked);
            }));

    // OSCプローブ名
    AddLabeledRow(LOCTEXT("OSCProbeName", "OSCプローブ名"),
        SNew(SEditableTextBox)
            .Text_Lambda([W]() -> FText {
                return (W.IsValid() && W->OSCOutput)
                    ? FText::FromString(W->OSCOutput->OSCProbeName)
                    : FText::GetEmpty();
            })
            .OnTextCommitted_Lambda([W](const FText& T, ETextCommit::Type) {
                if (W.IsValid() && W->OSCOutput)
                    W->OSCOutput->OSCProbeName = T.ToString();
            }));

    // バンドル送信
    AddLabeledRow(LOCTEXT("BundleMsg", "バンドル送信"),
        SNew(SCheckBox)
            .IsChecked_Lambda([W]() -> ECheckBoxState {
                return (W.IsValid() && W->OSCOutput && W->OSCOutput->bUseBundleMessage)
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([W](ECheckBoxState S) {
                if (W.IsValid() && W->OSCOutput)
                    W->OSCOutput->bUseBundleMessage = (S == ECheckBoxState::Checked);
            }));

    // --- ターゲット #1 ---
    if (Probe->OSCOutput->Targets.Num() == 0)
    {
        SettingsPanelBox->AddSlot().AutoHeight().Padding(8.0f, 2.0f)
            [SNew(STextBlock)
                .Text(LOCTEXT("NoTarget", "OSCターゲットがありません"))
                .ColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f)))];
        return;
    }

    SettingsPanelBox->AddSlot().AutoHeight().Padding(8.0f, 6.0f, 4.0f, 2.0f)
        [SNew(STextBlock)
            .Text(LOCTEXT("Target1Hdr", "ターゲット #1"))
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 10))];

    // ターゲット有効
    AddLabeledRow(LOCTEXT("TargetEnabled", "有効"),
        SNew(SCheckBox)
            .IsChecked_Lambda([W]() -> ECheckBoxState {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    return W->OSCOutput->Targets[0].bEnabled
                        ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                return ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([W](ECheckBoxState S) {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    W->OSCOutput->Targets[0].bEnabled = (S == ECheckBoxState::Checked);
            }));

    // ターゲット名
    AddLabeledRow(LOCTEXT("TargetName", "ターゲット名"),
        SNew(SEditableTextBox)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    return FText::FromString(W->OSCOutput->Targets[0].TargetName);
                return FText::GetEmpty();
            })
            .OnTextCommitted_Lambda([W](const FText& T, ETextCommit::Type) {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    W->OSCOutput->Targets[0].TargetName = T.ToString();
            }));

    // IPアドレス
    AddLabeledRow(LOCTEXT("OSCIP", "IPアドレス"),
        SNew(SEditableTextBox)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    return FText::FromString(W->OSCOutput->Targets[0].IPAddress);
                return FText::GetEmpty();
            })
            .OnTextCommitted_Lambda([W](const FText& T, ETextCommit::Type) {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                {
                    W->OSCOutput->Targets[0].IPAddress = T.ToString();
                    W->OSCOutput->ReconnectAll();
                }
            }));

    // ポート
    AddLabeledRow(LOCTEXT("OSCPort", "ポート"),
        SNew(SSpinBox<int32>)
            .MinValue(1).MaxValue(65535).Delta(1)
            .Value_Lambda([W]() -> int32 {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    return W->OSCOutput->Targets[0].Port;
                return 7700;
            })
            .OnValueChanged_Lambda([W](int32 V) {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                {
                    W->OSCOutput->Targets[0].Port = V;
                    W->OSCOutput->ReconnectAll();
                }
            }));

    // フォーマット (スピンボックス + ラベル)
    AddLabeledRow(LOCTEXT("OSCFormat", "フォーマット"),
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(0.35f)
            [SNew(SSpinBox<int32>)
                .MinValue(0).MaxValue(NumOSCFormats - 1).Delta(1)
                .Value_Lambda([W]() -> int32 {
                    if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                        return static_cast<int32>(W->OSCOutput->Targets[0].Format);
                    return 0;
                })
                .OnValueChanged_Lambda([W](int32 V) {
                    if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                        W->OSCOutput->Targets[0].Format = static_cast<EOSCMessageFormat>(V);
                })]
        + SHorizontalBox::Slot().FillWidth(0.65f).VAlign(VAlign_Center).Padding(6.0f, 0.0f)
            [SNew(STextBlock)
                .Text_Lambda([W]() -> FText {
                    if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    {
                        int32 Idx = FMath::Clamp(
                            static_cast<int32>(W->OSCOutput->Targets[0].Format),
                            0, NumOSCFormats - 1);
                        return FText::FromString(OSCFormatNames[Idx]);
                    }
                    return FText::GetEmpty();
                })]);

    // アドレス接頭辞
    AddLabeledRow(LOCTEXT("OSCPrefix", "アドレス接頭辞"),
        SNew(SEditableTextBox)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    return FText::FromString(W->OSCOutput->Targets[0].AddressPrefix);
                return FText::GetEmpty();
            })
            .OnTextCommitted_Lambda([W](const FText& T, ETextCommit::Type) {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    W->OSCOutput->Targets[0].AddressPrefix = T.ToString();
            }));
}

// ============================================================
// レイアウトヘルパー
// ============================================================

void SLightSyncMonitorWidget::AddSectionHeader(const FText& Title)
{
    SettingsPanelBox->AddSlot().AutoHeight()
        .Padding(2.0f, 10.0f, 2.0f, 2.0f)
        [SNew(STextBlock)
            .Text(Title)
            .Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))];

    SettingsPanelBox->AddSlot().AutoHeight()
        [SNew(SSeparator)];
}

void SLightSyncMonitorWidget::AddLabeledRow(const FText& Label, TSharedRef<SWidget> ValueWidget)
{
    SettingsPanelBox->AddSlot().AutoHeight()
        .Padding(8.0f, 2.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
                .FillWidth(0.4f)
                .VAlign(VAlign_Center)
                .Padding(2.0f)
                [SNew(STextBlock).Text(Label)]
            + SHorizontalBox::Slot()
                .FillWidth(0.6f)
                .VAlign(VAlign_Center)
                .Padding(2.0f)
                [ValueWidget]
        ];
}

// ============================================================
// ボタンコールバック
// ============================================================

FReply SLightSyncMonitorWidget::OnActivateAllClicked()
{
    if (ULightSyncSubsystem* Sub = GetSubsystem())
        Sub->SetAllProbesActive(true);
    RefreshProbeList();
    return FReply::Handled();
}

FReply SLightSyncMonitorWidget::OnDeactivateAllClicked()
{
    if (ULightSyncSubsystem* Sub = GetSubsystem())
        Sub->SetAllProbesActive(false);
    RefreshProbeList();
    return FReply::Handled();
}

FReply SLightSyncMonitorWidget::OnBlackoutClicked()
{
    if (ULightSyncSubsystem* Sub = GetSubsystem())
        Sub->BlackoutAll();
    RefreshProbeList();
    return FReply::Handled();
}

FReply SLightSyncMonitorWidget::OnRefreshClicked()
{
    RefreshProbeList();
    RefreshSettingsPanel();
    return FReply::Handled();
}

FReply SLightSyncMonitorWidget::OnForceSampleClicked()
{
    if (ULightSyncSubsystem* Sub = GetSubsystem())
        Sub->ForceSampleAll();
    RefreshProbeList();
    return FReply::Handled();
}

// ============================================================
// ユーティリティ
// ============================================================

ULightSyncSubsystem* SLightSyncMonitorWidget::GetSubsystem() const
{
    if (!GEditor)
    {
        return nullptr;
    }

    UWorld* World = nullptr;
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        if (Context.WorldType == EWorldType::PIE && Context.World())
        {
            World = Context.World();
            break;
        }
    }

    if (!World)
    {
        World = GEditor->GetEditorWorldContext().World();
    }

    return World ? World->GetSubsystem<ULightSyncSubsystem>() : nullptr;
}

TArray<ALightProbeActor*> SLightSyncMonitorWidget::GetAllProbeActors() const
{
    TArray<ALightProbeActor*> Result;

    UWorld* World = nullptr;
    if (GEditor)
    {
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            if (Context.WorldType == EWorldType::PIE && Context.World())
            {
                World = Context.World();
                break;
            }
        }
        if (!World)
        {
            World = GEditor->GetEditorWorldContext().World();
        }
    }

    if (World)
    {
        for (TActorIterator<ALightProbeActor> It(World); It; ++It)
        {
            if (IsValid(*It))
            {
                Result.Add(*It);
            }
        }
    }

    return Result;
}

ALightProbeActor* SLightSyncMonitorWidget::FindProbeByName(const FString& Name) const
{
    if (Name.IsEmpty())
    {
        return nullptr;
    }

    TArray<ALightProbeActor*> Probes = GetAllProbeActors();
    for (ALightProbeActor* Probe : Probes)
    {
        if (Probe && Probe->GetProbeName() == Name)
        {
            return Probe;
        }
    }

    return nullptr;
}

#undef LOCTEXT_NAMESPACE
