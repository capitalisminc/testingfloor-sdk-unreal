using UnrealBuildTool;

public class TestingFloorSdk : ModuleRules
{
	public TestingFloorSdk(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "Json" });
		PrivateDependencyModuleNames.Add("HTTP");
	}
}
