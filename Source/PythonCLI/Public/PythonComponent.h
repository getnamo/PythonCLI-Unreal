#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PythonTypes.h"
#include "PythonComponent.generated.h"

class FPythonProcess;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPythonScriptSignature, const FString&, Script);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPythonScriptErrorSignature, const FString&, Message, const FString&, Traceback);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPythonEventSignature, const FString&, EventName, const FString&, JsonArgs, const FPythonEventData&, Data);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPythonLogSignature, const FString&, Message, const FString&, Level);

/**
 * Runs a Python script (out of process) and talks to it: events both ways (JSON plus binary/tensors), and
 * function calls with replies. The script uses `import unreal_bridge as ue` (@ue.on, ue.emit, ...).
 * By default scripts run in the engine's shared, warm Python process; each component's scripts are
 * isolated (own module and worker thread) and unloaded when it ends.
 */
UCLASS(ClassGroup = "Python", meta = (BlueprintSpawnableComponent))
class PYTHONCLI_API UPythonComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UPythonComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python")
	FPythonScriptParams ScriptParams;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python")
	bool bStartScriptOnBeginPlay = true;

	/** Run in the engine's shared Python process (warm across PIE, imports paid once). Off: this component
	 gets its own process (isolated from crashes and from other scripts' GIL use; slower to start). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python")
	bool bUseSharedProcess = true;

	/** Also write the script's print/log output to the Output Log (LogPythonCLI). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python")
	bool bLogScriptOutput = true;

	UPROPERTY(BlueprintAssignable, Category = "Python Events")
	FPythonScriptSignature OnScriptStarted;

	UPROPERTY(BlueprintAssignable, Category = "Python Events")
	FPythonScriptSignature OnScriptStopped;

	/** The script failed to load, raised in a handler, or its process died. */
	UPROPERTY(BlueprintAssignable, Category = "Python Events")
	FPythonScriptErrorSignature OnScriptError;

	/** ue.emit(name, *args) from the script. */
	UPROPERTY(BlueprintAssignable, Category = "Python Events")
	FPythonEventSignature OnEvent;

	/** print() / ue.log from the script. Level is info, warning (stderr) or error. */
	UPROPERTY(BlueprintAssignable, Category = "Python Events")
	FPythonLogSignature OnLog;

	UFUNCTION(BlueprintCallable, Category = "Python")
	void StartScript();

	UFUNCTION(BlueprintCallable, Category = "Python")
	void StopScript();

	/** Re-read and restart the script (on_stop runs first). */
	UFUNCTION(BlueprintCallable, Category = "Python")
	void ReloadScript();

	UFUNCTION(BlueprintPure, Category = "Python")
	bool IsScriptRunning() const { return bScriptRunning; }

	/** Send an event to the script's @ue.on(EventName) handlers. JsonArgs: a JSON array of arguments (or a
	 single JSON value). */
	UFUNCTION(BlueprintCallable, Category = "Python", meta = (AutoCreateRefTerm = "JsonArgs"))
	void EmitEvent(const FString& EventName, const FString& JsonArgs);

	/** As EmitEvent; the tensors are appended as arguments (numpy arrays in Python) unless JsonArgs places
	 them with {"_tensor": i} placeholders. */
	UFUNCTION(BlueprintCallable, Category = "Python", meta = (AutoCreateRefTerm = "JsonArgs"))
	void EmitEventWithTensors(const FString& EventName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors);

	/** Call a function in the script (a @ue.on handler or a module-level function); OnResult runs on the game
	 thread with its return value or the Python error. Blueprint: Call Python Function. */
	void CallFunction(const FString& FunctionName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors, TFunction<void(const FPythonCallResult&)> OnResult);

	/** C++: handle one event name (game thread). */
	void BindEventNative(const FString& EventName, TFunction<void(const FPythonEventData&)> Handler);
	void UnbindEventNative(const FString& EventName);

	/** Full path of the script file to run (project scripts root, else the plugin's Content/Python). */
	FString ResolveScriptPath() const;

	/** Unique per component; scripts are keyed by it in the Python process. */
	const FString& GetOwnerId() const { return OwnerId; }

	/** The Python process this component's script runs in (shared or dedicated; created on first use). */
	TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> GetProcess();

	/** Process id of the Python process this component uses (0 if none yet). */
	UFUNCTION(BlueprintPure, Category = "Python")
	int32 GetPythonProcessId() const;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	bool EnsureProcess();
	void ReleaseProcess();
	void SendControl(const FString& Command);
	void SendEvent(const FString& EventName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors, int32 CallId);
	void HandleFrame(const struct FPythonFrame& Frame);
	void HandleProcessExit(int32 Code, const FString& Message);
	void FailPendingCalls(const FString& Error);

	FString OwnerId;
	TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> Process;
	bool bScriptRunning = false;
	bool bScriptStarting = false;
	int32 NextCallId = 0;
	TMap<int32, TFunction<void(const FPythonCallResult&)>> PendingCalls;
	TMap<FString, TFunction<void(const FPythonEventData&)>> NativeHandlers;
};
