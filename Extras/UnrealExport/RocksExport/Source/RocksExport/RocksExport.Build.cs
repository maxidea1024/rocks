// Unreal Engine module rules for the RocksExport editor plugin (Docs/Design/UnrealExchange.ko.md section 11.1 in the Rocks repository).
// Dependencies are added in the stage that uses them: UX1b needs the asset registry, the content browser menu and JSON parsing for
// its self tests; UX2a adds ImageCore (texture source mip images); mesh, material and baking modules come with later stages.

using UnrealBuildTool;

public class RocksExport : ModuleRules
{
	public RocksExport(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"ContentBrowser",
				"Core",
				"CoreUObject",
				"DesktopPlatform",
				"Engine",
				"ImageCore",
				"Json",
				"Projects",
				"Slate",
				"SlateCore",
				"ToolMenus",
				"UnrealEd",
			});
	}
}
