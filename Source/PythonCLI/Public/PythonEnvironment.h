#pragma once

#include "CoreMinimal.h"
#include "PythonEnvironment.generated.h"

UENUM(BlueprintType)
enum class EPythonEnvironmentState : uint8
{
	NotSetUp,
	SettingUp,
	Ready,
	Failed
};

/**
 * The bundled interpreter (python-build-standalone) and uv, and the project's venv built from them.
 * Python processes always run the bundled interpreter and add the venv's site-packages, so moving the
 * project never breaks them; the venv itself is only uv's install target (and something an IDE can use).
 * Setup runs on a background thread; callbacks and OnStateChanged fire on the game thread.
 */
class PYTHONCLI_API FPythonEnvironment : public TSharedFromThis<FPythonEnvironment, ESPMode::ThreadSafe>
{
public:
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnStateChanged, EPythonEnvironmentState /*State*/, const FString& /*Message*/);
	FOnStateChanged OnStateChanged;

	/** Requirements files other plugins add to every project's venv (e.g. a framework profile). Call from
	 a module's StartupModule; changes to the files are picked up on the next setup. */
	static void RegisterRequirementsFile(const FString& FilePath);
	static void UnregisterRequirementsFile(const FString& FilePath);

	/** Environment variables other plugins set for every Python process started from now on (e.g.
	 KERAS_BACKEND). The project's Environment Variables setting overrides them; an empty Value removes one. */
	static void RegisterEnvironmentVariable(const FString& Name, const FString& Value);
	static TMap<FString, FString> GetEnvironmentVariables();

	/** Script folders other plugins add (e.g. their bundled examples): components find scripts there after
	 the project's own, and processes put them on sys.path. Call from a module's StartupModule. */
	static void RegisterScriptDirectory(const FString& Directory);
	static TArray<FString> GetScriptDirectories();

	/** Create/update the venv if requirements changed (no-op when up to date), then call OnDone. Once Ready,
	 it doesn't look again unless bRecheck (e.g. after the requirements settings changed). */
	void EnsureReady(TFunction<void(bool bReady, const FString& Error)> OnDone = nullptr, bool bForceReinstall = false, bool bRecheck = false);

	EPythonEnvironmentState GetState() const;
	FString GetLastError() const;

	static FString PluginDir();
	static FString PluginPythonDir();
	static FString RuntimeScriptPath();
	static FString InterpreterPath();
	static FString UvPath();
	static bool HasBundledRuntime();
	static FString BundledRuntimeVersion();
	/** Editor: the venv (Venv Directory setting). Packaged with InstallOnFirstRun: Saved/PythonCLI/venv. */
	FString VenvDir() const;
	/** The venv's site-packages; in a packaged Bundle build, the packages staged with it
	 (Intermediate/PythonCLI/packaged/<Platform>/site-packages). */
	FString SitePackagesDir() const;
	/** Where runtime configs and generated files go: Intermediate/PythonCLI (editor), Saved/PythonCLI (packaged). */
	static FString WorkDir();
	/** True in packaged (non-editor) builds, where Project Settings > Python CLI > Packaged Env Mode applies. */
	static bool IsPackagedBuild();
	/** The file the requirements are combined into for uv. */
	FString RequirementsPath() const;

	/** Linux: archives made on Windows can lose the execute bit. */
	static void EnsureExecutable(const FString& Path);

private:
	bool RunSetup(bool bForceReinstall, FString& OutError);
	/** Runs a tool with output streamed to the log; returns its exit code (-1 if it couldn't start). */
	int32 RunTool(const FString& Exe, const FString& Args, const FString& WorkingDir);
	void Finish(bool bOk, const FString& Error);

	mutable FCriticalSection Lock;
	EPythonEnvironmentState State = EPythonEnvironmentState::NotSetUp;
	FString LastError;
	TArray<TFunction<void(bool, const FString&)>> Waiting;
};
