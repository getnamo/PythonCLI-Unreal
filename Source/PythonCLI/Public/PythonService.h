#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "PythonComponent.h"
#include "PythonTypes.h"
#include "PythonService.generated.h"

class FPythonProcess;
class UPythonJob;

/** Which Python process a service runs in. */
UENUM(BlueprintType)
enum class EPythonServiceProcess : uint8
{
	/** The engine's shared, warm process (imports and models load once per editor session). */
	Shared,
	/** Its own process: isolated from crashes and from other scripts' GIL use (e.g. a training job). */
	Dedicated,
};

DECLARE_MULTICAST_DELEGATE_OneParam(FPythonServiceNativeEvent, const FPythonEventData& /*Event*/);

/**
 * A named Python script host that isn't tied to an actor: any Blueprint, component or system can call it,
 * stream to it, or start jobs in it. Services live as long as the engine (across PIE sessions), like the
 * shared process. Get one with UPythonSubsystem::GetService. The script uses unreal_bridge as usual.
 */
UCLASS(BlueprintType)
class PYTHONCLI_API UPythonService : public UObject
{
	GENERATED_BODY()
public:
	UPythonService();

	/** Set by UPythonSubsystem::GetService before the first Start. */
	void Configure(const FString& InName, const FPythonScriptParams& InScript, EPythonServiceProcess InProcess);

	UFUNCTION(BlueprintPure, Category = "Python Service")
	const FString& GetServiceName() const { return ServiceName; }

	UPROPERTY(BlueprintReadOnly, Category = "Python Service")
	FPythonScriptParams ScriptParams;

	UPROPERTY(BlueprintReadOnly, Category = "Python Service")
	EPythonServiceProcess ProcessMode = EPythonServiceProcess::Shared;

	/** Also write the script's print/log output to the Output Log (LogPythonCLI). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Service")
	bool bLogScriptOutput = true;

	UPROPERTY(BlueprintAssignable, Category = "Python Service")
	FPythonScriptSignature OnScriptStarted;

	UPROPERTY(BlueprintAssignable, Category = "Python Service")
	FPythonScriptSignature OnScriptStopped;

	/** The script failed to load, raised in a handler, or its process died. */
	UPROPERTY(BlueprintAssignable, Category = "Python Service")
	FPythonScriptErrorSignature OnScriptError;

	/** ue.emit(name, *args) from the script. */
	UPROPERTY(BlueprintAssignable, Category = "Python Service")
	FPythonEventSignature OnEvent;

	UPROPERTY(BlueprintAssignable, Category = "Python Service")
	FPythonLogSignature OnLog;

	/** Load the script (no-op while it runs or starts). Calls made before it has started wait for it. */
	UFUNCTION(BlueprintCallable, Category = "Python Service")
	void Start();

	/** Unload the script (on_stop runs; its jobs are stopped first). */
	UFUNCTION(BlueprintCallable, Category = "Python Service")
	void Stop();

	/** Re-read and restart the script. */
	UFUNCTION(BlueprintCallable, Category = "Python Service")
	void Reload();

	UFUNCTION(BlueprintPure, Category = "Python Service")
	bool IsRunning() const { return bScriptRunning; }

	UFUNCTION(BlueprintPure, Category = "Python Service")
	bool IsStarting() const { return bScriptStarting; }

	/** Send an event to the script's @ue.on(EventName) handlers. */
	UFUNCTION(BlueprintCallable, Category = "Python Service", meta = (AutoCreateRefTerm = "JsonArgs,Tensors"))
	void EmitEvent(const FString& EventName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors);

	/** Call a function in the script; OnResult runs on the game thread. Starts the script if needed. Blueprint:
	 Call Python Service. */
	void CallFunction(const FString& FunctionName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors, TFunction<void(const FPythonCallResult&)> OnResult);

	/** C++: handlers for one event name (any number), on the game thread. */
	FPythonServiceNativeEvent& OnNativeEvent(const FString& EventName) { return NativeEvents.FindOrAdd(EventName); }

	/** Start a @ue.job in the script. ParamsJson: a JSON object of the job's parameters. With bResume the
	 job continues its latest run. RunsDir: where runs go (empty: Saved/PythonCLI/Runs). */
	UFUNCTION(BlueprintCallable, Category = "Python Service", meta = (AutoCreateRefTerm = "ParamsJson,RunsDir"))
	UPythonJob* StartJob(const FString& JobName, const FString& ParamsJson, bool bResume, const FString& RunsDir);

	/** Jobs started here that haven't finished. */
	UFUNCTION(BlueprintPure, Category = "Python Service")
	TArray<UPythonJob*> GetRunningJobs() const;

	/** The script's @ue.job functions and their parameters, as JSON (an array of {name, params, doc}). */
	void ListJobs(TFunction<void(const FPythonCallResult&)> OnResult);

	/** The process the script runs in (created on first use). */
	TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> GetProcess();
	const FString& GetOwnerId() const { return OwnerId; }
	FString ResolveScriptPath() const;

	UFUNCTION(BlueprintPure, Category = "Python Service")
	int32 GetPythonProcessId() const;

	/** Stop the script and release the process (dedicated processes exit). UPythonSubsystem calls it. */
	void Shutdown();

	virtual void BeginDestroy() override;

private:
	friend class UPythonJob;
	bool EnsureProcess();
	void SendControl(const FString& Command);
	void SendEvent(const FString& EventName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors, int32 CallId);
	void HandleFrame(const struct FPythonFrame& Frame);
	void HandleProcessExit(int32 Code, const FString& Message);
	void FailPendingCalls(const FString& Error);
	void StopJob(const FString& Id);

	FString ServiceName;
	FString OwnerId;
	TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> Process;
	bool bScriptRunning = false;
	bool bScriptStarting = false;
	int32 NextCallId = 0;
	int32 NextJobId = 0;
	TMap<int32, TFunction<void(const FPythonCallResult&)>> PendingCalls;
	TMap<FString, FPythonServiceNativeEvent> NativeEvents;

	UPROPERTY()
	TMap<FString, TObjectPtr<UPythonJob>> Jobs;
};
