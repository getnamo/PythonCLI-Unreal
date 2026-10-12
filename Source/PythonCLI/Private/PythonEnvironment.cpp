#include "PythonEnvironment.h"
#include "PythonCLI.h"
#include "PythonCLISettings.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_UNIX
#include <sys/stat.h>
#endif

namespace PythonEnvironmentPrivate
{
	static FCriticalSection RegisteredLock;
	static TArray<FString> RegisteredFiles;

	static FString Quote(const FString& S)
	{
		return TEXT("\"") + S + TEXT("\"");
	}

	static bool LongPathsEnabled()
	{
#if PLATFORM_WINDOWS
		::DWORD Value = 0;
		::DWORD Size = sizeof(Value);
		return ::RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"LongPathsEnabled",
			RRF_RT_REG_DWORD, nullptr, &Value, &Size) == ERROR_SUCCESS && Value == 1;
#else
		return true;
#endif
	}

	/** Longest relative path seen in TensorFlow's wheel (its C++ headers); MAX_PATH is 260. */
	constexpr int32 LongestPackagePath = 165;
}

void FPythonEnvironment::RegisterRequirementsFile(const FString& FilePath)
{
	FScopeLock L(&PythonEnvironmentPrivate::RegisteredLock);
	PythonEnvironmentPrivate::RegisteredFiles.AddUnique(FPaths::ConvertRelativePathToFull(FilePath));
}

FString FPythonEnvironment::PluginDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PythonCLI"));
	return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()) : FString();
}

FString FPythonEnvironment::PluginPythonDir()
{
	return PluginDir() / TEXT("Content/Python");
}

FString FPythonEnvironment::RuntimeScriptPath()
{
	return PluginPythonDir() / TEXT("runtime.py");
}

FString FPythonEnvironment::InterpreterPath()
{
#if PLATFORM_WINDOWS
	return PluginDir() / TEXT("ThirdParty/python/Win64/python.exe");
#else
	return PluginDir() / TEXT("ThirdParty/python/Linux/bin/python3.12");
#endif
}

FString FPythonEnvironment::UvPath()
{
#if PLATFORM_WINDOWS
	return PluginDir() / TEXT("ThirdParty/uv/Win64/uv.exe");
#else
	return PluginDir() / TEXT("ThirdParty/uv/Linux/uv");
#endif
}

bool FPythonEnvironment::HasBundledRuntime()
{
	return FPaths::FileExists(InterpreterPath());
}

FString FPythonEnvironment::BundledRuntimeVersion()
{
	FString Version;
	FFileHelper::LoadFileToString(Version, *(FPaths::GetPath(InterpreterPath()) /
#if PLATFORM_WINDOWS
		TEXT(".runtime-version")));
#else
		TEXT("../.runtime-version")));
#endif
	return Version.TrimStartAndEnd();
}

FString FPythonEnvironment::VenvDir() const
{
	const FString Configured = GetDefault<UPythonCLISettings>()->VenvDirectory;
	const FString Dir = FPaths::IsRelative(Configured) ? FPaths::ProjectDir() / Configured : Configured;
	return FPaths::ConvertRelativePathToFull(Dir);
}

FString FPythonEnvironment::SitePackagesDir() const
{
#if PLATFORM_WINDOWS
	return VenvDir() / TEXT("Lib/site-packages");
#else
	return VenvDir() / TEXT("lib/python3.12/site-packages");
#endif
}

FString FPythonEnvironment::RequirementsPath() const
{
	return FPaths::GetPath(VenvDir()) / TEXT("requirements.txt");
}

void FPythonEnvironment::EnsureExecutable(const FString& Path)
{
#if PLATFORM_UNIX
	struct stat St;
	const FTCHARToUTF8 P(*Path);
	if (::stat(P.Get(), &St) == 0 && (St.st_mode & S_IXUSR) == 0)
	{
		::chmod(P.Get(), St.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
	}
#endif
}

EPythonEnvironmentState FPythonEnvironment::GetState() const
{
	FScopeLock L(&Lock);
	return State;
}

FString FPythonEnvironment::GetLastError() const
{
	FScopeLock L(&Lock);
	return LastError;
}

void FPythonEnvironment::EnsureReady(TFunction<void(bool bReady, const FString& Error)> OnDone, bool bForceReinstall, bool bRecheck)
{
	{
		FScopeLock L(&Lock);
		if (State == EPythonEnvironmentState::Ready && !bForceReinstall && !bRecheck)
		{
			if (OnDone)
			{
				OnDone(true, FString());
			}
			return;
		}
		if (OnDone)
		{
			Waiting.Add(MoveTemp(OnDone));
		}
		if (State == EPythonEnvironmentState::SettingUp)
		{
			return;
		}
		State = EPythonEnvironmentState::SettingUp;
	}
	OnStateChanged.Broadcast(EPythonEnvironmentState::SettingUp, TEXT("Setting up the Python environment"));

	TSharedPtr<FPythonEnvironment, ESPMode::ThreadSafe> Self = AsShared();
	Async(EAsyncExecution::Thread, [Self, bForceReinstall]
	{
		FString Error;
		const bool bOk = Self->RunSetup(bForceReinstall, Error);
		AsyncTask(ENamedThreads::GameThread, [Self, bOk, Error]
		{
			Self->Finish(bOk, Error);
		});
	});
}

void FPythonEnvironment::Finish(bool bOk, const FString& Error)
{
	TArray<TFunction<void(bool, const FString&)>> Callbacks;
	{
		FScopeLock L(&Lock);
		State = bOk ? EPythonEnvironmentState::Ready : EPythonEnvironmentState::Failed;
		LastError = Error;
		Callbacks = MoveTemp(Waiting);
	}
	if (bOk)
	{
		UE_LOG(LogPythonCLI, Display, TEXT("Python environment ready (%s)"), *VenvDir());
	}
	else
	{
		UE_LOG(LogPythonCLI, Error, TEXT("Python environment setup failed: %s"), *Error);
	}
	OnStateChanged.Broadcast(bOk ? EPythonEnvironmentState::Ready : EPythonEnvironmentState::Failed, bOk ? VenvDir() : Error);
	for (TFunction<void(bool, const FString&)>& Callback : Callbacks)
	{
		Callback(bOk, Error);
	}
}

bool FPythonEnvironment::RunSetup(bool bForceReinstall, FString& OutError)
{
	using namespace PythonEnvironmentPrivate;
	const UPythonCLISettings* Settings = GetDefault<UPythonCLISettings>();
	const FString Interpreter = InterpreterPath();
	const FString Uv = UvPath();
	if (!FPaths::FileExists(Interpreter) || !FPaths::FileExists(Uv))
	{
		OutError = FString::Printf(TEXT("The bundled Python runtime is missing (%s). Run Tools/FetchRuntime.ps1 (or Tools/fetch_runtime.sh) in the PythonCLI plugin."), *Interpreter);
		return false;
	}
	EnsureExecutable(Interpreter);
	EnsureExecutable(Uv);

	// Requirements: settings + files registered by plugins + the project's scripts requirements.txt
	FString Requirements;
	FString HashInput = BundledRuntimeVersion() + TEXT("\n");
	for (const FString& Line : Settings->Requirements)
	{
		const FString Trimmed = Line.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			Requirements += Trimmed + LINE_TERMINATOR;
		}
	}
	TArray<FString> Files;
	{
		FScopeLock L(&RegisteredLock);
		Files = RegisteredFiles;
	}
	Files.Add(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Content/Scripts/requirements.txt")));
	for (const FString& File : Files)
	{
		FString Contents;
		if (FFileHelper::LoadFileToString(Contents, *File) && !Contents.TrimStartAndEnd().IsEmpty())
		{
			Requirements += TEXT("-r ") + Quote(File) + LINE_TERMINATOR;
			HashInput += File + TEXT("\n") + Contents + TEXT("\n");
		}
	}
	FString IndexArgs;
	if (!Settings->IndexUrl.IsEmpty())
	{
		IndexArgs += TEXT(" --index-url ") + Quote(Settings->IndexUrl);
	}
	for (const FString& Url : Settings->ExtraIndexUrls)
	{
		IndexArgs += TEXT(" --extra-index-url ") + Quote(Url);
	}
	if (!Settings->TorchBackend.IsEmpty())
	{
		IndexArgs += TEXT(" --torch-backend ") + Settings->TorchBackend;
	}
	HashInput += Requirements + IndexArgs;
	const FTCHARToUTF8 HashUtf8(*HashInput);
	const FString Hash = FSHA1::HashBuffer(HashUtf8.Get(), HashUtf8.Length()).ToString();

	const FString Venv = VenvDir();
	const FString Marker = Venv / TEXT("pythoncli-env.txt");
	FString Existing;
	if (!bForceReinstall && FFileHelper::LoadFileToString(Existing, *Marker) && Existing.TrimStartAndEnd() == Hash && FPaths::DirectoryExists(SitePackagesDir()))
	{
		UE_LOG(LogPythonCLI, Log, TEXT("Python environment up to date (%s)"), *Venv);
		return true;
	}

	if (!LongPathsEnabled() && SitePackagesDir().Len() + 1 + LongestPackagePath > 259)
	{
		UE_LOG(LogPythonCLI, Warning,
			TEXT("The venv's site-packages path is %d characters and Windows long paths are off: packages with deep paths (e.g. TensorFlow) may fail to install. ")
			TEXT("Enable long paths (HKLM\\SYSTEM\\CurrentControlSet\\Control\\FileSystem\\LongPathsEnabled = 1) or set a shorter Venv Directory in Project Settings > Plugins > Python CLI."),
			SitePackagesDir().Len());
	}

	const FString WorkDir = FPaths::GetPath(Venv);
	IFileManager::Get().MakeDirectory(*WorkDir, true);
	const FString Common = TEXT("--no-progress --color never --no-config");

	// Create the venv if needed, or recreate it when the bundled Python changed (the marker records it)
	const bool bHaveVenv = FPaths::FileExists(Venv / TEXT("pyvenv.cfg"));
	FString VenvPython;
	FFileHelper::LoadFileToString(VenvPython, *(Venv / TEXT("pythoncli-python.txt")));
	if (!bHaveVenv || VenvPython != BundledRuntimeVersion())
	{
		UE_LOG(LogPythonCLI, Display, TEXT("Creating venv %s"), *Venv);
		const FString Args = FString::Printf(TEXT("venv %s --python %s --clear %s"), *Common, *Quote(Interpreter), *Quote(Venv));
		if (RunTool(Uv, Args, WorkDir) != 0)
		{
			OutError = TEXT("uv venv failed (see the log)");
			return false;
		}
		FFileHelper::SaveStringToFile(BundledRuntimeVersion(), *(Venv / TEXT("pythoncli-python.txt")));
	}

	FFileHelper::SaveStringToFile(Requirements, *RequirementsPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	if (!Requirements.IsEmpty())
	{
		UE_LOG(LogPythonCLI, Display, TEXT("Installing Python requirements into %s"), *Venv);
		const FString Args = FString::Printf(TEXT("pip install %s --python %s -r %s%s%s"), *Common, *Quote(Venv), *Quote(RequirementsPath()),
			*IndexArgs, bForceReinstall ? TEXT(" --reinstall") : TEXT(""));
		if (RunTool(Uv, Args, WorkDir) != 0)
		{
			OutError = TEXT("uv pip install failed (see the log)");
			return false;
		}
	}
	FFileHelper::SaveStringToFile(Hash, *Marker);
	return true;
}

int32 FPythonEnvironment::RunTool(const FString& Exe, const FString& Args, const FString& WorkingDir)
{
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
	FProcHandle Proc = FPlatformProcess::CreateProc(*Exe, *Args, false, true, true, nullptr, 0, *WorkingDir, WritePipe, nullptr);
	if (!Proc.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		UE_LOG(LogPythonCLI, Error, TEXT("Could not run %s %s"), *Exe, *Args);
		return -1;
	}

	FString Pending;
	auto Pump = [&Pending, ReadPipe](bool bFinal)
	{
		Pending += FPlatformProcess::ReadPipe(ReadPipe);
		int32 Newline;
		while (Pending.FindChar(TEXT('\n'), Newline))
		{
			const FString Line = Pending.Left(Newline).TrimEnd();
			Pending.RightChopInline(Newline + 1);
			if (!Line.IsEmpty())
			{
				UE_LOG(LogPythonCLI, Display, TEXT("[uv] %s"), *Line);
			}
		}
		if (bFinal && !Pending.TrimStartAndEnd().IsEmpty())
		{
			UE_LOG(LogPythonCLI, Display, TEXT("[uv] %s"), *Pending.TrimStartAndEnd());
			Pending.Reset();
		}
	};
	while (FPlatformProcess::IsProcRunning(Proc))
	{
		Pump(false);
		FPlatformProcess::Sleep(0.05f);
	}
	Pump(true);

	int32 Code = -1;
	FPlatformProcess::GetProcReturnCode(Proc, &Code);
	FPlatformProcess::CloseProc(Proc);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
	return Code;
}
