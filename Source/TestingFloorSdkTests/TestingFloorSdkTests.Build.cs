using UnrealBuildTool;

public class TestingFloorSdkTests : ModuleRules
{
	public TestingFloorSdkTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "EngineSettings", "Json", "Projects", "TestingFloorSdk" });
	}
}
