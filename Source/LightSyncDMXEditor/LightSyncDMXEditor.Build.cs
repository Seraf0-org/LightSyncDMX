// LightSyncDMXEditor - Editor Build Configuration

using UnrealBuildTool;

public class LightSyncDMXEditor : ModuleRules
{
    public LightSyncDMXEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "LightSyncDMX",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
            "UnrealEd",
            "ToolMenus",
            "WorkspaceMenuStructure",
            "LevelEditor",
            "PropertyEditor",
        });
    }
}
