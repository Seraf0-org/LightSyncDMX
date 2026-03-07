// Copyright UE-Comp. All Rights Reserved.

#include "LightSyncDMXEditorModule.h"
#include "SLightSyncMonitorWidget.h"

#include "ToolMenus.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "FLightSyncDMXEditorModule"

static const FName LightSyncTabName("LightSyncDMXMonitor");

void FLightSyncDMXEditorModule::StartupModule()
{
    // グローバルタブを登録
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
                                LightSyncTabName,
                                FOnSpawnTab::CreateLambda([](const FSpawnTabArgs &Args) -> TSharedRef<SDockTab>
                                                          { return SNew(SDockTab)
                                                                .TabRole(ETabRole::NomadTab)
                                                                .Label(LOCTEXT("TabTitle", "LightSync Monitor"))
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
