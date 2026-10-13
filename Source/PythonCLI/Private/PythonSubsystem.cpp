#include "PythonSubsystem.h"
#include "PythonProcess.h"
#include "PythonCLI.h"
#include "PythonCLISettings.h"
#include "Engine/Engine.h"
#include "Misc/CommandLine.h"

UPythonSubsystem* UPythonSubsystem::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<UPythonSubsystem>() : nullptr;
}

void UPythonSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Environment = MakeShared<FPythonEnvironment, ESPMode::ThreadSafe>();
	Environment->OnStateChanged.AddWeakLambda(this, [this](EPythonEnvironmentState State, const FString& Message)
	{
		OnEnvironmentStateChanged.Broadcast(State, Message);
	});

	const UPythonCLISettings* Settings = GetDefault<UPythonCLISettings>();
	if (FPythonEnvironment::IsPackagedBuild() && Settings->PackagedEnvMode == EPythonPackagedEnvMode::None)
	{
		UE_LOG(LogPythonCLI, Log, TEXT("Python is not part of this build (Packaged Env Mode: None)"));
		return;
	}
	if (!FPythonEnvironment::HasBundledRuntime())
	{
		UE_LOG(LogPythonCLI, Warning, TEXT("The bundled Python runtime is missing: run Tools/FetchRuntime.ps1 (or Tools/fetch_runtime.sh) in %s"), *FPythonEnvironment::PluginDir());
		return;
	}
	//Not for commandlets (cooking, asset tools): they don't run scripts
	if (Settings->bSetupEnvironmentOnStartup && !IsRunningCommandlet())
	{
		Environment->EnsureReady([this](bool bOk, const FString&)
		{
			if (bOk && GIsEditor && GetDefault<UPythonCLISettings>()->bStartSharedProcessOnStartup)
			{
				GetSharedProcess();
			}
		});
	}
}

void UPythonSubsystem::Deinitialize()
{
	if (SharedProcess.IsValid())
	{
		SharedProcess->Stop(GetDefault<UPythonCLISettings>()->ShutdownGraceSeconds, /*bWait*/ true);
		SharedProcess.Reset();
	}
	Super::Deinitialize();
}

TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> UPythonSubsystem::GetSharedProcess()
{
	if (!SharedProcess.IsValid())
	{
		SharedProcess = MakeShared<FPythonProcess, ESPMode::ThreadSafe>(TEXT("shared"), /*bPreload*/ true);
	}
	if (!SharedProcess->IsStarting() && !SharedProcess->IsRunning())
	{
		SharedProcess->Start(Environment);
	}
	return SharedProcess;
}

TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> UPythonSubsystem::CreateDedicatedProcess(const FString& Name)
{
	TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> Process = MakeShared<FPythonProcess, ESPMode::ThreadSafe>(Name);
	Process->Start(Environment);
	return Process;
}

void UPythonSubsystem::SetupEnvironment(bool bForceReinstall)
{
	Environment->EnsureReady(nullptr, bForceReinstall, /*bRecheck*/ true);
}

void UPythonSubsystem::RestartSharedProcess()
{
	if (SharedProcess.IsValid())
	{
		SharedProcess->Stop(GetDefault<UPythonCLISettings>()->ShutdownGraceSeconds, /*bWait*/ false);
	}
}

EPythonEnvironmentState UPythonSubsystem::GetEnvironmentState() const
{
	return Environment.IsValid() ? Environment->GetState() : EPythonEnvironmentState::NotSetUp;
}

bool UPythonSubsystem::IsSharedProcessReady() const
{
	return SharedProcess.IsValid() && SharedProcess->IsReady();
}
