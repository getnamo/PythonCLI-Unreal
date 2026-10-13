using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using EpicGames.Core;
using Microsoft.Extensions.Logging;
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
			"RenderCore",
			"RHI",
			"AudioCaptureCore",
		});

		string PluginDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
		string PlatformDir = Target.Platform == UnrealTargetPlatform.Win64 ? "Win64" : Target.Platform == UnrealTargetPlatform.Linux ? "Linux" : null;
		if (PlatformDir == null)
		{
			return;
		}
		bool bPackaged = Target.Type != TargetType.Editor && Target.ProjectFile != null;
		string Mode = bPackaged ? ReadSetting(Target, "PackagedEnvMode", "Bundle") : "";
		if (Mode == "None")
		{
			return; // No Python in this build
		}

		// The bridge scripts and this platform's interpreter (+ uv where the game installs packages itself)
		RuntimeDependencies.Add(Path.Combine(PluginDir, "Content", "Python", "..."), StagedFileType.NonUFS);
		RuntimeDependencies.Add(Path.Combine(PluginDir, "ThirdParty", "python", PlatformDir, "..."), StagedFileType.NonUFS);
		if (!bPackaged || Mode == "InstallOnFirstRun")
		{
			RuntimeDependencies.Add(Path.Combine(PluginDir, "ThirdParty", "uv", PlatformDir, "..."), StagedFileType.NonUFS);
		}
		if (!bPackaged)
		{
			return;
		}

		// Packaged builds: the project's scripts, and with Bundle the environment's packages for this platform
		string ProjectDir = Target.ProjectFile.Directory.FullName;
		if (Directory.Exists(Path.Combine(ProjectDir, "Content", "Scripts")))
		{
			RuntimeDependencies.Add("$(ProjectDir)/Content/Scripts/...", StagedFileType.NonUFS);
		}
		if (Mode == "Bundle")
		{
			StageBundledPackages(Target, PluginDir, ProjectDir, PlatformDir);
		}
	}

	string ReadSetting(ReadOnlyTargetRules Target, string Key, string Default)
	{
		ConfigHierarchy Config = ConfigCache.ReadHierarchy(ConfigHierarchyType.Game, Target.ProjectFile.Directory, Target.Platform);
		string Value;
		return Config.GetString("/Script/PythonCLI.PythonCLISettings", Key, out Value) && !string.IsNullOrEmpty(Value) ? Value : Default;
	}

	List<string> ReadSettingArray(ReadOnlyTargetRules Target, string Key)
	{
		ConfigHierarchy Config = ConfigCache.ReadHierarchy(ConfigHierarchyType.Game, Target.ProjectFile.Directory, Target.Platform);
		List<string> Values;
		return Config.GetArray("/Script/PythonCLI.PythonCLISettings", Key, out Values) ? Values : new List<string>();
	}

	/// Installs the packages the editor's environment uses (Intermediate/PythonCLI/requirements.txt, written by
	/// its last setup) for the target platform with uv (cross-platform: --python-platform), into
	/// Intermediate/PythonCLI/packaged/<Platform>/site-packages, and stages them from there (same path in the build).
	/// Reinstalls only when the requirements (or the interpreter) change.
	void StageBundledPackages(ReadOnlyTargetRules Target, string PluginDir, string ProjectDir, string PlatformDir)
	{
		string HostDir = BuildHostPlatform.Current.Platform == UnrealTargetPlatform.Win64 ? "Win64" : "Linux";
		string HostPython = HostDir == "Win64" ? Path.Combine(PluginDir, "ThirdParty", "python", "Win64", "python.exe")
			: Path.Combine(PluginDir, "ThirdParty", "python", "Linux", "bin", "python3.12");
		string Uv = Path.Combine(PluginDir, "ThirdParty", "uv", HostDir, HostDir == "Win64" ? "uv.exe" : "uv");
		string Requirements = Path.Combine(ProjectDir, "Intermediate", "PythonCLI", "requirements.txt");
		string OutDir = Path.Combine(ProjectDir, "Intermediate", "PythonCLI", "packaged", PlatformDir);
		string SiteDir = Path.Combine(OutDir, "site-packages");
		string Marker = Path.Combine(OutDir, "pythoncli-packaged.txt");

		string RequirementsText = File.Exists(Requirements) ? File.ReadAllText(Requirements) : "";
		if (!File.Exists(Requirements))
		{
			Logger.LogWarning("PythonCLI: no {File}: open the project in the editor once (its environment setup writes it); bundling no packages", Requirements);
		}
		// The hash covers the requirements, the files they include (-r), the index options and the interpreter
		StringBuilder HashInput = new StringBuilder(PlatformDir + "\n" + RequirementsText);
		foreach (string Line in RequirementsText.Split('\n').Select(L => L.Trim()).Where(L => L.StartsWith("-r ")))
		{
			string Included = Line.Substring(3).Trim().Trim('"');
			HashInput.Append(File.Exists(Included) ? File.ReadAllText(Included) : "?");
		}
		List<string> IndexArgs = new List<string>();
		string IndexUrl = ReadSetting(Target, "IndexUrl", "");
		if (IndexUrl.Length > 0)
		{
			IndexArgs.Add("--index-url \"" + IndexUrl + "\"");
		}
		foreach (string Url in ReadSettingArray(Target, "ExtraIndexUrls"))
		{
			IndexArgs.Add("--extra-index-url \"" + Url + "\"");
		}
		string TorchBackend = ReadSetting(Target, "TorchBackend", "");
		if (TorchBackend.Length > 0)
		{
			IndexArgs.Add("--torch-backend " + TorchBackend);
		}
		string RuntimeStamp = Path.Combine(PluginDir, "ThirdParty", "python", HostDir, ".runtime-version");
		HashInput.Append(string.Join(" ", IndexArgs)).Append(File.Exists(RuntimeStamp) ? File.ReadAllText(RuntimeStamp) : "");
		string Hash = Convert.ToHexString(SHA1.HashData(Encoding.UTF8.GetBytes(HashInput.ToString())));

		bool bUpToDate = File.Exists(Marker) && File.ReadAllText(Marker).Trim() == Hash && Directory.Exists(SiteDir);
		if (!bUpToDate)
		{
			if (Directory.Exists(SiteDir))
			{
				Directory.Delete(SiteDir, true);
			}
			Directory.CreateDirectory(SiteDir);
			if (RequirementsText.Trim().Length > 0)
			{
				if (!File.Exists(Uv) || !File.Exists(HostPython))
				{
					throw new BuildException("PythonCLI: the bundled runtime is missing ({0}); run Tools/FetchRuntime.ps1 (or fetch_runtime.sh)", Uv);
				}
				if (!OperatingSystem.IsWindows())
				{
					// Archives made on Windows (the release .7z) don't keep the executable bit
					foreach (string Executable in new string[] { Uv, HostPython })
					{
						File.SetUnixFileMode(Executable, File.GetUnixFileMode(Executable) | UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute);
					}
				}
				string TargetPlatform = PlatformDir == "Win64" ? "x86_64-pc-windows-msvc" : "x86_64-manylinux_2_28";
				string Args = string.Format("pip install --python \"{0}\" --python-platform {1} --python-version 3.12 --target \"{2}\" --no-progress --color never --no-config -r \"{3}\" {4}",
					HostPython, TargetPlatform, SiteDir, Requirements, string.Join(" ", IndexArgs));
				Logger.LogInformation("PythonCLI: installing the Python packages for {Platform} (Packaged Env Mode: Bundle)...", PlatformDir);
				ProcessStartInfo Start = new ProcessStartInfo(Uv, Args) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true };
				List<string> Output = new List<string>();
				using (Process Installer = new Process { StartInfo = Start })
				{
					// Both streams at once: reading one to its end first can deadlock on the other
					DataReceivedEventHandler Collect = (Sender, E) => { if (!string.IsNullOrWhiteSpace(E.Data)) { lock (Output) { Output.Add(E.Data.TrimEnd()); } } };
					Installer.OutputDataReceived += Collect;
					Installer.ErrorDataReceived += Collect;
					Installer.Start();
					Installer.BeginOutputReadLine();
					Installer.BeginErrorReadLine();
					Installer.WaitForExit();
					foreach (string Line in Output.TakeLast(Installer.ExitCode == 0 ? 6 : 40))
					{
						Logger.LogInformation("  [uv] {Line}", Line);
					}
					if (Installer.ExitCode != 0)
					{
						throw new BuildException("PythonCLI: uv failed ({0}) installing the bundled packages for {1}", Installer.ExitCode, PlatformDir);
					}
				}
			}
			File.WriteAllText(Marker, Hash);
		}

		// Stage it all except C headers and bytecode caches (TensorFlow's headers alone are 160+ characters deep)
		int Staged = 0;
		foreach (string FilePath in Directory.EnumerateFiles(SiteDir, "*", SearchOption.AllDirectories))
		{
			string Relative = Path.GetRelativePath(SiteDir, FilePath);
			string[] Parts = Relative.Split(new char[] { '\\', '/' });
			if (Parts.Any(P => P == "__pycache__" || P == "include"))
			{
				continue;
			}
			// Staged where it is (a source path would make UBT copy it into the project first)
			RuntimeDependencies.Add("$(ProjectDir)/Intermediate/PythonCLI/packaged/" + PlatformDir + "/site-packages/" + string.Join("/", Parts), StagedFileType.NonUFS);
			++Staged;
		}
		Logger.LogInformation("PythonCLI: bundling {Count} Python package files for {Platform}{State}", Staged, PlatformDir, bUpToDate ? " (up to date)" : "");
	}
}
