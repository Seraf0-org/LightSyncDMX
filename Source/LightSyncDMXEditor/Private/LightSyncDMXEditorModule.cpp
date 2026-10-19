// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncDMXEditorModule.h"
#include "SLightSyncMonitorWidget.h"

#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SWindow.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "LightSyncDMXModule.h"

#define LOCTEXT_NAMESPACE "FLightSyncDMXEditorModule"

static const FName LightSyncTabName("LightSyncDMXMonitor");

// LightSync.OpenMonitor [ProbeName] [Width Height]: モニターを開き、プローブを選ぶ
static FAutoConsoleCommand GLightSyncOpenMonitor(
    TEXT("LightSync.OpenMonitor"),
    TEXT("LightSync DMX Monitor を開く。引数: [プローブ名] [幅 高さ]"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
    {
        FGlobalTabmanager::Get()->TryInvokeTab(LightSyncTabName);
        TSharedPtr<SLightSyncMonitorWidget> Monitor = SLightSyncMonitorWidget::LastInstance.Pin();
        if (!Monitor.IsValid())
        {
            return;
        }
        if (Args.Num() >= 1)
        {
            Monitor->SelectProbeByName(Args[0]);
        }
        if (Args.Num() >= 3)
        {
            if (TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(Monitor.ToSharedRef()))
            {
                Window->Resize(FVector2D(FCString::Atof(*Args[1]), FCString::Atof(*Args[2])));
            }
        }
    }));

// LightSync.SnapshotMonitor <png パス> [設定パネルのスクロール位置]: モニターを PNG に保存
static FAutoConsoleCommand GLightSyncSnapshotMonitor(
    TEXT("LightSync.SnapshotMonitor"),
    TEXT("LightSync DMX Monitor の見た目を PNG に保存する。引数: <パス> [スクロール位置]"),
    FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
    {
        TSharedPtr<SLightSyncMonitorWidget> Monitor = SLightSyncMonitorWidget::LastInstance.Pin();
        if (!Monitor.IsValid() || Args.Num() < 1)
        {
            UE_LOG(LogLightSyncDMX, Warning, TEXT("SnapshotMonitor: モニターが開いていないか、パスがありません"));
            return;
        }
        if (Args.Num() >= 2)
        {
            Monitor->SetSettingsScrollOffset(FCString::Atof(*Args[1]));
            // スクロールを反映させてから撮る
            FSlateApplication::Get().Tick();
        }

        TArray<FColor> Pixels;
        FIntVector Size;
        if (!FSlateApplication::Get().TakeScreenshot(Monitor.ToSharedRef(), Pixels, Size) || Pixels.Num() == 0)
        {
            UE_LOG(LogLightSyncDMX, Warning, TEXT("SnapshotMonitor: スクリーンショットに失敗"));
            return;
        }
        for (FColor& Pixel : Pixels)
        {
            Pixel.A = 255;
        }
        TArray64<uint8> Png;
        FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
        FFileHelper::SaveArrayToFile(Png, *Args[0]);
        UE_LOG(LogLightSyncDMX, Display, TEXT("LIGHTSYNC_SNAPSHOT %s (%dx%d)"), *Args[0], Size.X, Size.Y);
    }));

void FLightSyncDMXEditorModule::StartupModule()
{
    // グローバルタブを登録
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
                                LightSyncTabName,
                                FOnSpawnTab::CreateLambda([](const FSpawnTabArgs &Args) -> TSharedRef<SDockTab>
                                                          { return SNew(SDockTab)
                                                                .TabRole(ETabRole::NomadTab)
                                                                .Label(LOCTEXT("TabTitle", "LightSync DMX Monitor"))
                                                                    [SNew(SLightSyncMonitorWidget)]; }))
        .SetDisplayName(LOCTEXT("TabDisplayName", "LightSync DMX Monitor"))
        .SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsMiscCategory())
        .SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

    RegisterMenuExtensions();
}

void FLightSyncDMXEditorModule::ShutdownModule()
{
    UnregisterMenuExtensions();

    FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LightSyncTabName);
}

void FLightSyncDMXEditorModule::RegisterMenuExtensions()
{
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
                                                          {
			UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
			if (Menu)
			{
				FToolMenuSection& Section = Menu->FindOrAddSection("VirtualProduction");
				Section.Label = LOCTEXT("VPSection", "Virtual Production");

				Section.AddMenuEntry(
					"OpenLightSyncMonitor",
					LOCTEXT("MenuLabel", "LightSync DMX Monitor"),
					LOCTEXT("MenuTooltip", "UEの光色 → DMX 同期モニタを開く"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([]()
					{
						FGlobalTabmanager::Get()->TryInvokeTab(LightSyncTabName);
					}))
				);
			} }));
}

void FLightSyncDMXEditorModule::UnregisterMenuExtensions()
{
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FLightSyncDMXEditorModule, LightSyncDMXEditor)
