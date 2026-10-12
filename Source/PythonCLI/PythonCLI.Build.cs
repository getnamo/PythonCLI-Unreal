using System.IO;
using UnrealBuildTool;

public class PythonCLI : ModuleRules
{
	public PythonCLI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"DeveloperSettings",
			"CLISystem",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
		});

		// Stage the bridge scripts and this platform's interpreter + uv with packaged builds.
		string PluginDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		RuntimeDependencies.Add(Path.Combine(PluginDir, "Content", "Python", "..."));
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			RuntimeDependencies.Add(Path.Combine(PluginDir, "ThirdParty", "python", "Win64", "..."));
			RuntimeDependencies.Add(Path.Combine(PluginDir, "ThirdParty", "uv", "Win64", "..."));
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			RuntimeDependencies.Add(Path.Combine(PluginDir, "ThirdParty", "python", "Linux", "..."));
			RuntimeDependencies.Add(Path.Combine(PluginDir, "ThirdParty", "uv", "Linux", "..."));
		}
	}
}
