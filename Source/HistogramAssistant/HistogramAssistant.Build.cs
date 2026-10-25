using UnrealBuildTool;

public class HistogramAssistant : ModuleRules
{
	public HistogramAssistant(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"LevelEditor",
			"DesktopPlatform",
			"WorkspaceMenuStructure",
			"RenderCore"
		});
	}
}
