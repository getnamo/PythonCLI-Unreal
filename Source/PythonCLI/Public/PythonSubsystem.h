#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "PythonEnvironment.h"
#include "PythonSubsystem.generated.h"

class FPythonProcess;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPythonEnvironmentStateSignature, EPythonEnvironmentState, State, const FString&, Message);

/**
 * Owns the Python environment (venv setup) and the shared Python process. Engine-lifetime: the shared
 * process stays warm across PIE sessions, so heavy imports (numpy, torch, tensorflow) and model loads
 * happen once per editor session. Components get their scripts unloaded when they end.
 */
UCLASS()
class PYTHONCLI_API UPythonSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()
public:
	static UPythonSubsystem* Get();

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	TSharedPtr<FPythonEnvironment, ESPMode::ThreadSafe> GetEnvironment() const { return Environment; }

	/** The shared process, started if it isn't running. */
	TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> GetSharedProcess();

	/** A new process just for one owner (bUseSharedProcess off). Started; stop it when done. */
	TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> CreateDedicatedProcess(const FString& Name);

	/** Bring the venv in line with the requirements now (normally automatic: at startup, and when the
	 requirement settings change in the editor). bForceReinstall reinstalls every requirement. */
	UFUNCTION(BlueprintCallable, Category = "Python CLI")
	void SetupEnvironment(bool bForceReinstall = false);

	/** Stop the shared process (scripts get on_stop); components using it start it again on next use. */
	UFUNCTION(BlueprintCallable, Category = "Python CLI")
	void RestartSharedProcess();

	UFUNCTION(BlueprintPure, Category = "Python CLI")
	EPythonEnvironmentState GetEnvironmentState() const;

	UFUNCTION(BlueprintPure, Category = "Python CLI")
	bool IsSharedProcessReady() const;

	UPROPERTY(BlueprintAssignable, Category = "Python CLI")
	FPythonEnvironmentStateSignature OnEnvironmentStateChanged;

private:
	TSharedPtr<FPythonEnvironment, ESPMode::ThreadSafe> Environment;
	TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> SharedProcess;
};
