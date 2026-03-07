// LightSyncDMX - Build Configuration

using UnrealBuildTool;

public class LightSyncDMX : ModuleRules
{
    public LightSyncDMX(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "RenderCore",
            "RHI",
            "Renderer",
            "DMXProtocol",
            "DMXRuntime",
            "OSC",
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
        });
    }
}
