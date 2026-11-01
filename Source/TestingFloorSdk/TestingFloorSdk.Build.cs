using UnrealBuildTool;

public class TestingFloorSdk : ModuleRules
{
	public TestingFloorSdk(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Json" });
		PrivateDependencyModuleNames.AddRange(new string[] { "EngineSettings", "HTTP" });
	}
}
