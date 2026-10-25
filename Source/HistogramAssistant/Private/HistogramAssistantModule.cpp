#include "HistogramAssistantModule.h"

#include "HistogramPanel.h"
#include "HistogramComparePanel.h"
#include "Framework/Docking/TabManager.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "FHistogramAssistantModule"

const FName FHistogramAssistantModule::TabName = TEXT("HistogramAssistantTab");
const FName FHistogramAssistantModule::CompareTabName = TEXT("HistogramAssistantCompareTab");

void FHistogramAssistantModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		TabName,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs& Args) -> TSharedRef<SDockTab>
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				.Label(LOCTEXT("TabLabel", "直方图助手"))
				[
					SNew(SHistogramPanel)
				];
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "直方图助手"))
		.SetTooltipText(LOCTEXT("TabTooltip", "读取编辑器活动视口并显示 RGB/亮度直方图"))
		.SetMenuType(ETabSpawnerMenuType::Enabled)
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());

	// 帧对比（A/B）窗口：独立 tab，可以拖出来放到副屏（编辑器布局会记住位置）
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		CompareTabName,
		FOnSpawnTab::CreateLambda([](const FSpawnTabArgs& Args) -> TSharedRef<SDockTab>
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				.Label(LOCTEXT("CompareTabLabel", "帧对比（A/B）"))
				[
					SNew(SFrameComparePanel)
				];
		}))
		.SetDisplayName(LOCTEXT("CompareTabTitle", "直方图助手 · 帧对比（A/B）"))
		.SetTooltipText(LOCTEXT("CompareTabTooltip", "参考帧 / 当前帧 对比窗口（并排 / 擦除 / 差异），适合拖到副屏"))
		.SetMenuType(ETabSpawnerMenuType::Enabled)
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory());
}

void FHistogramAssistantModule::ShutdownModule()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(CompareTabName);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FHistogramAssistantModule, HistogramAssistant)
