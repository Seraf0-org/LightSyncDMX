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
    TEXT("CCT+Brightness (2ch)"),
    TEXT("CCT & RGBW (10ch)")
};
static constexpr int32 NumColorModes = UE_ARRAY_COUNT(ColorModeNames);
// ELightSyncColorMode に値を追加したら ColorModeNames も同時に更新すること
static_assert(NumColorModes == 6, "ColorModeNames と ELightSyncColorMode の要素数が一致していません");

// 正規化照度 (1.0 を超えうる) を色見本用に縮める。色相は保つ
static FLinearColor ToDisplayColor(const FLinearColor& C)
{
    const float MaxComp = FMath::Max3(C.R, C.G, C.B);
    FLinearColor Out = MaxComp > 1.0f ? C / MaxComp : C;
    Out.A = 1.0f;
    return Out;
}

// モニターからの編集をレベルの未保存変更として扱う (保存し忘れで設定が消えないように)
static void MarkProbeDirty(const TWeakObjectPtr<ALightProbeActor>& W)
{
    if (W.IsValid())
    {
        W->MarkPackageDirty();
    }
}

static const TCHAR* OSCFormatNames[] = {
    TEXT("Float RGB"),
    TEXT("Int RGB (0-255)"),
    TEXT("Float RGBA"),
    TEXT("QLC+ 互換"),
    TEXT("DasLight 互換"),
    TEXT("カスタム")
};
static constexpr int32 NumOSCFormats = UE_ARRAY_COUNT(OSCFormatNames);
// EOSCMessageFormat に値を追加したら OSCFormatNames も同時に更新すること
static_assert(NumOSCFormats == 6, "OSCFormatNames と EOSCMessageFormat の要素数が一致していません");

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
        // プローブ数が変わった時だけリストを再構築
        TArray<ALightProbeActor*> CurrentProbes = GetAllProbeActors();
        if (CurrentProbes.Num() != CachedProbeCount)
        {
            CachedProbeCount = CurrentProbes.Num();
            RefreshProbeList();
        }
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
                    })]]

        + SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 2.0f).VAlign(VAlign_Center)
            [SNew(SCheckBox)
                .ToolTipText(LOCTEXT("DrawDebugTip", "ビューポートに各プローブの出力色を球で表示"))
                .IsChecked_Lambda([this]() -> ECheckBoxState {
                    const ULightSyncSubsystem* Sub = GetSubsystem();
                    return (Sub && Sub->bDrawDebug) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([this](ECheckBoxState S) {
                    if (ULightSyncSubsystem* Sub = GetSubsystem())
                    {
                        Sub->bDrawDebug = (S == ECheckBoxState::Checked);
                    }
                })
                [SNew(STextBlock).Text(LOCTEXT("DrawDebug", "デバッグ表示"))]];
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
        TWeakObjectPtr<ALightProbeActor> WeakProbe = Probe;

        // DMX情報 (静的)
        FString DMXInfo = TEXT("-");
        if (Probe->bUseDMXOutput && Probe->DMXOutput
            && Probe->DMXOutput->FixtureMappings.Num() > 0)
        {
            const FDMXFixtureMapping& M = Probe->DMXOutput->FixtureMappings[0];
            DMXInfo = FString::Printf(TEXT("U%d Ch%d"), M.Universe, M.StartChannel);
        }

        // OSC情報 (静的)
        const FString OSCInfo = Probe->bUseOSCOutput ? TEXT("ON") : TEXT("-");

        // 位置 (静的)
        const FVector Loc = Probe->GetActorLocation();
        const FString LocText = FString::Printf(TEXT("%.0f, %.0f, %.0f"), Loc.X, Loc.Y, Loc.Z);

        ProbeListBox->AddSlot().AutoHeight()
            [
                SNew(SBorder)
                    .BorderBackgroundColor_Lambda([this, ProbeName]() {
                        const bool bIsSelected = (ProbeName == SelectedProbeName);
                        return FSlateColor(bIsSelected
                            ? FLinearColor(0.12f, 0.22f, 0.42f)
                            : FLinearColor(0.03f, 0.03f, 0.03f));
                    })
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

                                // 状態 (動的)
                                + SHorizontalBox::Slot().FillWidth(0.06f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(STextBlock)
                                        .Text_Lambda([WeakProbe]() {
                                            if (WeakProbe.IsValid())
                                            {
                                                return WeakProbe->bIsProbeActive
                                                    ? LOCTEXT("Active", "●")
                                                    : LOCTEXT("Inactive", "○");
                                            }
                                            return LOCTEXT("Invalid", "-");
                                        })
                                        .ColorAndOpacity_Lambda([WeakProbe]() {
                                            if (WeakProbe.IsValid() && WeakProbe->bIsProbeActive)
                                            {
                                                return FSlateColor(FLinearColor::Green);
                                            }
                                            return FSlateColor(FLinearColor::Gray);
                                        })]

                                // 色プレビュー (動的)
                                + SHorizontalBox::Slot().FillWidth(0.06f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(SBox).WidthOverride(32.0f).HeightOverride(16.0f)
                                        [SNew(SColorBlock)
                                            .Color_Lambda([WeakProbe]() {
                                                if (WeakProbe.IsValid())
                                                {
                                                    return ToDisplayColor(WeakProbe->GetCurrentColor());
                                                }
                                                return FLinearColor::Black;
                                            })
                                            .ShowBackgroundForAlpha(false)]]

                                // RGB (動的)
                                + SHorizontalBox::Slot().FillWidth(0.20f).Padding(2.0f)
                                    .VAlign(VAlign_Center)
                                    [SNew(STextBlock)
                                        .Text_Lambda([WeakProbe]() {
                                            if (WeakProbe.IsValid())
                                            {
                                                const FLinearColor Color = WeakProbe->GetCurrentColor();
                                                return FText::FromString(FString::Printf(TEXT("%.2f %.2f %.2f"),
                                                    Color.R, Color.G, Color.B));
                                            }
                                            return FText::FromString(TEXT("-"));
                                        })]

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
    BuildReferenceSection(Probe);
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
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
                if (W.IsValid()) W->SetProbeActive(S == ECheckBoxState::Checked);
            }));

    // サンプリングレート
    AddLabeledRow(LOCTEXT("SampRate", "サンプリングレート (Hz)"),
        SNew(SSpinBox<float>)
            .MinValue(1.0f).MaxValue(120.0f).Delta(1.0f)
            .Value_Lambda([W]() { return W.IsValid() ? W->SamplingRate : 30.0f; })
            .OnValueChanged_Lambda([W](float V) { MarkProbeDirty(W); if (W.IsValid()) W->SamplingRate = V; }));

    // キャプチャ解像度
    AddLabeledRow(LOCTEXT("CaptureRes", "キャプチャ解像度"),
        SNew(SSpinBox<int32>)
            .MinValue(16).MaxValue(512).Delta(16)
            .Value_Lambda([W]() { return W.IsValid() ? W->CaptureResolution : 64; })
            .OnValueChanged_Lambda([W](int32 V) { MarkProbeDirty(W); if (W.IsValid()) W->CaptureResolution = V; }));

    // DMX出力トグル
    AddLabeledRow(LOCTEXT("UseDMX", "DMX出力を使用"),
        SNew(SCheckBox)
            .IsChecked_Lambda([W]() -> ECheckBoxState {
                return (W.IsValid() && W->bUseDMXOutput)
                    ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
            })
            .OnCheckStateChanged_Lambda([W](ECheckBoxState S) {
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
                if (W.IsValid()) W->bUseOSCOutput = (S == ECheckBoxState::Checked);
            }));
}

// ============================================================
// 色補正セクション
// ============================================================

void SLightSyncMonitorWidget::BuildReferenceSection(ALightProbeActor* Probe)
{
    TWeakObjectPtr<ALightProbeActor> W(Probe);

    AddSectionHeader(LOCTEXT("SecReference", "色の基準"));

    static const TCHAR* ReferenceModeNames[] = {
        TEXT("LEDウォール表示色 (ディスプレイ基準)"),
        TEXT("シーンの光 (シーン基準)"),
    };
    AddLabeledRow(LOCTEXT("RefMode", "基準モード"),
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(0.3f)
            [SNew(SSpinBox<int32>)
                .MinValue(0).MaxValue(1).Delta(1)
                .Value_Lambda([W]() { return W.IsValid() ? static_cast<int32>(W->ReferenceMode) : 0; })
                .OnValueChanged_Lambda([W](int32 V) {
                    MarkProbeDirty(W);
                    if (W.IsValid()) W->ReferenceMode = static_cast<ELightSyncReferenceMode>(FMath::Clamp(V, 0, 1));
                })]
        + SHorizontalBox::Slot().FillWidth(0.7f).VAlign(VAlign_Center).Padding(6.0f, 0.0f)
            [SNew(STextBlock)
                .Text_Lambda([W]() -> FText {
                    const int32 Idx = W.IsValid() ? FMath::Clamp(static_cast<int32>(W->ReferenceMode), 0, 1) : 0;
                    return FText::FromString(ReferenceModeNames[Idx]);
                })]);

    AddLabeledRow(LOCTEXT("RefCamera", "参照カメラ"),
        SNew(STextBlock)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->ReferenceCamera)
                    return FText::FromString(W->ReferenceCamera->GetActorNameOrLabel());
                return LOCTEXT("NoRefCamera", "なし (手動露出を使用。Details パネルで設定)");
            }));

    AddLabeledRow(LOCTEXT("ManualEV", "手動露出 EV100"),
        SNew(SSpinBox<float>)
            .MinValue(-10.0f).MaxValue(20.0f).Delta(0.1f)
            .Value_Lambda([W]() { return W.IsValid() ? W->ManualEV100 : 0.0f; })
            .OnValueChanged_Lambda([W](float V) { MarkProbeDirty(W); if (W.IsValid()) W->ManualEV100 = V; }));

    AddLabeledRow(LOCTEXT("ManualBias", "手動露出補正 (stop)"),
        SNew(SSpinBox<float>)
            .MinValue(-15.0f).MaxValue(15.0f).Delta(0.1f)
            .Value_Lambda([W]() { return W.IsValid() ? W->ManualExposureBias : 1.0f; })
            .OnValueChanged_Lambda([W](float V) { MarkProbeDirty(W); if (W.IsValid()) W->ManualExposureBias = V; }));

    AddLabeledRow(LOCTEXT("OutputGain", "出力ゲイン"),
        SNew(SSpinBox<float>)
            .MinValue(0.0f).MaxValue(100.0f).Delta(0.05f)
            .Value_Lambda([W]() { return W.IsValid() ? W->OutputGain : 1.0f; })
            .OnValueChanged_Lambda([W](float V) { MarkProbeDirty(W); if (W.IsValid()) W->OutputGain = V; }));

    AddLabeledRow(LOCTEXT("Smoothing", "スムージング時定数 (秒)"),
        SNew(SSpinBox<float>)
            .MinValue(0.0f).MaxValue(10.0f).Delta(0.01f)
            .Value_Lambda([W]() { return (W.IsValid() && W->ColorSampler) ? W->ColorSampler->SmoothingTimeConstant : 0.15f; })
            .OnValueChanged_Lambda([W](float V) {
                MarkProbeDirty(W);
                if (W.IsValid() && W->ColorSampler) W->ColorSampler->SmoothingTimeConstant = V;
            }));

    // === ゾーンごとの結果 ===
    AddSectionHeader(LOCTEXT("SecZones", "ゾーン (正規化照度、リニア。1.0 = 基準白の一様な環境)"));

    if (!Probe->ColorSampler)
    {
        return;
    }

    for (const FLightSyncZone& Zone : Probe->ColorSampler->GetActiveZones())
    {
        const FName ZoneName = Zone.Name;
        const FString Label = FString::Printf(TEXT("%s  (%s)"), *ZoneName.ToString(),
            *StaticEnum<ELightSyncZoneShape>()->GetDisplayNameTextByValue(static_cast<int64>(Zone.Shape)).ToString());

        AddLabeledRow(FText::FromString(Label),
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
                [SNew(SBox).WidthOverride(48.0f).HeightOverride(24.0f)
                    [SNew(SColorBlock)
                        .Color_Lambda([W, ZoneName]() {
                            FLinearColor C = FLinearColor::Black;
                            if (W.IsValid()) W->GetZoneColor(ZoneName, C);
                            return ToDisplayColor(C);
                        })
                        .ShowBackgroundForAlpha(false)]]
            + SHorizontalBox::Slot().AutoWidth().Padding(8.0f, 0.0f, 0.0f, 0.0f).VAlign(VAlign_Center)
                [SNew(STextBlock)
                    .Text_Lambda([W, ZoneName]() -> FText {
                        FLinearColor C = FLinearColor::Black;
                        if (!W.IsValid() || !W->GetZoneColor(ZoneName, C)) return LOCTEXT("ZoneNoResult", "(結果待ち)");
                        return FText::FromString(FString::Printf(TEXT("R %.3f  G %.3f  B %.3f"), C.R, C.G, C.B));
                    })]);
    }
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
                MarkProbeDirty(W);
                if (W.IsValid() && W->DMXOutput)
                    W->DMXOutput->bDMXOutputEnabled = (S == ECheckBoxState::Checked);
            }));

    // ゾーン (フィクスチャ 1 個目)
    AddLabeledRow(LOCTEXT("DMXZone", "ゾーン"),
        SNew(SEditableTextBox)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    return FText::FromName(W->DMXOutput->FixtureMappings[0].Zone);
                return FText::GetEmpty();
            })
            .OnTextCommitted_Lambda([W](const FText& T, ETextCommit::Type) {
                MarkProbeDirty(W);
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    W->DMXOutput->FixtureMappings[0].Zone = FName(*T.ToString());
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
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
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
                    MarkProbeDirty(W);
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

    // 使用チャンネル範囲 (512ch を超える場合は赤で警告)
    AddLabeledRow(LOCTEXT("ChannelRange", "使用チャンネル"),
        SNew(STextBlock)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                {
                    const FDMXFixtureMapping& M = W->DMXOutput->FixtureMappings[0];
                    const int32 EndCh = M.StartChannel + UDMXColorOutputComponent::GetMappingChannelCount(M) - 1;
                    return FText::FromString(UDMXColorOutputComponent::IsMappingInRange(M)
                        ? FString::Printf(TEXT("Ch %d - %d"), M.StartChannel, EndCh)
                        : FString::Printf(TEXT("Ch %d - %d (512超え: 送信されません)"), M.StartChannel, EndCh));
                }
                return FText::GetEmpty();
            })
            .ColorAndOpacity_Lambda([W]() -> FSlateColor {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0
                    && !UDMXColorOutputComponent::IsMappingInRange(W->DMXOutput->FixtureMappings[0]))
                {
                    return FSlateColor(FLinearColor(1.0f, 0.3f, 0.3f));
                }
                return FSlateColor::UseForeground();
            }));

    // 明度スケール
    AddLabeledRow(LOCTEXT("BrightScale", "強さのゲイン"),
        SNew(SSpinBox<float>)
            .MinValue(0.0f).MaxValue(100.0f).Delta(0.05f)
            .Value_Lambda([W]() -> float {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    return W->DMXOutput->FixtureMappings[0].BrightnessScale;
                return 1.0f;
            })
            .OnValueChanged_Lambda([W](float V) {
                MarkProbeDirty(W);
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                    W->DMXOutput->FixtureMappings[0].BrightnessScale = V;
            }));

    // プロファイル
    AddLabeledRow(LOCTEXT("DMXProfile", "灯体プロファイル"),
        SNew(STextBlock)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureMappings.Num() > 0)
                {
                    const FDMXFixtureMapping& M = W->DMXOutput->FixtureMappings[0];
                    if (M.Profile)
                        return FText::FromString(M.Profile->GetName());
                    return FText::FromString(FString::Printf(TEXT("内蔵プリセット (未キャリブレーション): %s"),
                        *StaticEnum<ELightSyncColorMode>()->GetDisplayNameTextByValue(static_cast<int64>(M.ColorMode)).ToString()));
                }
                return FText::GetEmpty();
            }));

    // ソルバの出力 (送信値と状態)
    AddLabeledRow(LOCTEXT("DMXOutputStatus", "出力"),
        SNew(STextBlock)
            .Text_Lambda([W]() -> FText {
                if (!W.IsValid() || !W->DMXOutput || W->DMXOutput->FixtureStatus.Num() == 0)
                    return LOCTEXT("DMXNoStatus", "(未送信)");
                const FLightSyncFixtureStatus& St = W->DMXOutput->FixtureStatus[0];
                FString Values;
                for (uint8 V : St.Values)
                {
                    Values += FString::Printf(TEXT("%d "), V);
                }
                FString Flags;
                if (St.bOutOfGamut) Flags += TEXT("  [色域外: 彩度を下げて出力]");
                if (St.bClipped) Flags += TEXT("  [明るさ上限]");
                return FText::FromString(FString::Printf(TEXT("%s (強さ %.0f%%)%s"), *Values.TrimEnd(), St.Intensity * 100.0f, *Flags));
            })
            .ColorAndOpacity_Lambda([W]() -> FSlateColor {
                if (W.IsValid() && W->DMXOutput && W->DMXOutput->FixtureStatus.Num() > 0
                    && (W->DMXOutput->FixtureStatus[0].bOutOfGamut || W->DMXOutput->FixtureStatus[0].bClipped))
                    return FSlateColor(FLinearColor(1.0f, 0.75f, 0.3f));
                return FSlateColor::UseForeground();
            }));

    // マスターディマー (個別)
    AddLabeledRow(LOCTEXT("DMXDimmer", "ディマー"),
        SNew(SSpinBox<float>)
            .MinValue(0.0f).MaxValue(1.0f).Delta(0.01f)
            .Value_Lambda([W]() -> float {
                return (W.IsValid() && W->DMXOutput) ? W->DMXOutput->MasterDimmer : 1.0f;
            })
            .OnValueChanged_Lambda([W](float V) {
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    W->OSCOutput->Targets[0].TargetName = T.ToString();
            }));

    // IPアドレス
    AddLabeledRow(LOCTEXT("OSCZone", "ゾーン"),
        SNew(SEditableTextBox)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    return FText::FromName(W->OSCOutput->Targets[0].Zone);
                return FText::GetEmpty();
            })
            .OnTextCommitted_Lambda([W](const FText& T, ETextCommit::Type) {
                MarkProbeDirty(W);
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    W->OSCOutput->Targets[0].Zone = FName(*T.ToString());
            }));

    AddLabeledRow(LOCTEXT("OSCIP", "IPアドレス"),
        SNew(SEditableTextBox)
            .Text_Lambda([W]() -> FText {
                if (W.IsValid() && W->OSCOutput && W->OSCOutput->Targets.Num() > 0)
                    return FText::FromString(W->OSCOutput->Targets[0].IPAddress);
                return FText::GetEmpty();
            })
            .OnTextCommitted_Lambda([W](const FText& T, ETextCommit::Type) {
                MarkProbeDirty(W);
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
                MarkProbeDirty(W);
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
                    MarkProbeDirty(W);
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
                MarkProbeDirty(W);
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
