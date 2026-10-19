// LightSyncDMX - Build Configuration

using UnrealBuildTool;

public class LightSyncDMX : ModuleRules
{
    public LightSyncDMX(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Cpp20;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "RenderCore",
            "RHI",
            "Renderer",
            "Projects",       // IPluginManager (シェーダーディレクトリマッピング用)
            "DMXProtocol",
            "DMXRuntime",
            "OSC",
            "OpenColorIO",    // nDisplay / ビューポートと同じ OCIO 変換をキャプチャに掛ける
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Slate",
            "SlateCore",
            "AssetRegistry",
            "DeveloperSettings",
            "Sockets",
            "Networking",
        });
    }
}
