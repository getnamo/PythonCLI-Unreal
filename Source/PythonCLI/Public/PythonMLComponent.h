#pragma once

#include "CoreMinimal.h"
#include "PythonComponent.h"
#include "PythonMLComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FPythonMLSignature);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPythonMLResultSignature, const FString&, JsonResult, const FString&, FunctionName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPythonMLFloatsSignature, const TArray<float>&, Result, const FString&, FunctionName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPythonMLJsonSignature, const FString&, Json);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPythonMLTrainingCompleteSignature, const FString&, JsonSummary, bool, bStoppedEarly);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPythonMLCustomEventSignature, const FString&, EventName, const FString&, Data);

/**
 * A Python component for machine-learning scripts. The script's get_api() returns an
 * unreal_bridge.ml.MLPluginAPI (or TFPluginAPI) instance and this component drives it: on_setup when the
 * script starts, inputs with results, training on its own Python thread (progress, early stop, completion)
 * and custom events (call_event). Script failures, including training exceptions, reach OnScriptError.
 */
UCLASS(ClassGroup = "Python", meta = (BlueprintSpawnableComponent))
class PYTHONCLI_API UPythonMLComponent : public UPythonComponent
{
	GENERATED_BODY()
public:
	/** Start training as soon as the script is set up (on its first start). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python ML")
	bool bTrainOnBeginPlay = false;

	/** On EndPlay, ask training to stop (should_stop) before the script unloads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python ML")
	bool bStopTrainingOnEndPlay = true;

	UPROPERTY(BlueprintReadOnly, Category = "Python ML")
	bool bIsSetUp = false;

	UPROPERTY(BlueprintReadOnly, Category = "Python ML")
	bool bIsTraining = false;

	/** on_setup finished: the API takes inputs and training now. */
	UPROPERTY(BlueprintAssignable, Category = "Python ML Events")
	FPythonMLSignature OnSetupComplete;

	/** The result of SendJsonInput, SendStringInput, SendTensorInput or CallCustomFunction, as JSON. */
	UPROPERTY(BlueprintAssignable, Category = "Python ML Events")
	FPythonMLResultSignature OnInputResults;

	/** The result of SendFloatArrayInput. */
	UPROPERTY(BlueprintAssignable, Category = "Python ML Events")
	FPythonMLFloatsSignature OnFloatArrayResults;

	UPROPERTY(BlueprintAssignable, Category = "Python ML Events")
	FPythonMLSignature OnTrainingStarted;

	/** report_progress(data) or ue.keras.ProgressEvents, as JSON (e.g. {"epoch": 2, "loss": 0.08}). */
	UPROPERTY(BlueprintAssignable, Category = "Python ML Events")
	FPythonMLJsonSignature OnTrainingProgress;

	/** on_begin_training returned: its return value as JSON. bStoppedEarly: StopTraining (or a script stop)
	 was requested. A training exception goes to OnScriptError instead. */
	UPROPERTY(BlueprintAssignable, Category = "Python ML Events")
	FPythonMLTrainingCompleteSignature OnTrainingComplete;

	/** call_event(name, data) from the script. Data: a string as it is, any other value as JSON. */
	UPROPERTY(BlueprintAssignable, Category = "Python ML Events")
	FPythonMLCustomEventSignature OnCustomEvent;

	/** on_json_input(data), or the API's FunctionName(data). Json: any JSON value. Result: OnInputResults. */
	UFUNCTION(BlueprintCallable, Category = "Python ML", meta = (AutoCreateRefTerm = "FunctionName"))
	void SendJsonInput(const FString& Json, const FString& FunctionName);

	/** As SendJsonInput, with a plain string. */
	UFUNCTION(BlueprintCallable, Category = "Python ML", meta = (AutoCreateRefTerm = "FunctionName"))
	void SendStringInput(const FString& Text, const FString& FunctionName);

	/** on_float_array_input(list of floats). Result: OnFloatArrayResults. */
	UFUNCTION(BlueprintCallable, Category = "Python ML", meta = (AutoCreateRefTerm = "FunctionName"))
	void SendFloatArrayInput(const TArray<float>& Values, const FString& FunctionName);

	/** on_tensor_input(array): a numpy array, zero-copy. Result: OnInputResults (Send Tensor Input (Async) also
	 returns tensors). */
	UFUNCTION(BlueprintCallable, Category = "Python ML", meta = (AutoCreateRefTerm = "FunctionName"))
	void SendTensorInput(const FPyTensor& Tensor, const FString& FunctionName);

	/** Call a method of the API object. JsonArgs: a JSON array of arguments, or one value. Result: OnInputResults. */
	UFUNCTION(BlueprintCallable, Category = "Python ML", meta = (AutoCreateRefTerm = "JsonArgs"))
	void CallCustomFunction(const FString& FunctionName, const FString& JsonArgs);

	/** on_begin_training on its own Python thread; inputs keep being answered meanwhile. */
	UFUNCTION(BlueprintCallable, Category = "Python ML")
	void BeginTraining();

	/** Sets should_stop (ue.keras.StopOnRequest ends fit()) and calls on_stop_training. */
	UFUNCTION(BlueprintCallable, Category = "Python ML")
	void StopTraining();

	UFUNCTION(BlueprintCallable, Category = "Python ML")
	void SaveModel(const FString& ModelName);

	UFUNCTION(BlueprintCallable, Category = "Python ML")
	void LoadModel(const FString& ModelName);

	/** C++ (and the async nodes): an ML call with the full result instead of the events. Op: "json_input",
	 "string_input", "float_input", "tensor_input", "custom", "save_model", "load_model". For inputs, Name is
	 an optional function name; for custom it's the method. */
	void CallML(const FString& Op, const FString& Name, const TSharedPtr<FJsonValue>& Input, const TArray<FPyTensor>& Tensors, TFunction<void(const FPythonCallResult&)> OnResult);

	/** Result as floats: a returned array/tensor, or a JSON list of numbers. */
	static TArray<float> ResultToFloats(const FPythonCallResult& Result);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	virtual void HandleScriptStarted(const FString& Script) override;
	virtual void HandleScriptStopped(const FString& Script) override;
	virtual bool HandleScriptEvent(const FPythonEventData& Data) override;

private:
	void CallReserved(const FString& Function, const TArray<TSharedPtr<FJsonValue>>& Args, const TArray<FPyTensor>& Tensors, TFunction<void(const FPythonCallResult&)> OnResult);
	TFunction<void(const FPythonCallResult&)> ResultHandler(const FString& FunctionName, bool bFloats);

	bool bTrainPending = false;
};
