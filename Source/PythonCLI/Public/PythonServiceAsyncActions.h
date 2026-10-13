#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "PythonCallAsyncAction.h"
#include "PythonTypes.h"
#include "PythonServiceAsyncActions.generated.h"

class UPythonJob;
class UPythonService;

/** Blueprint: Call Python Service, with OnSuccess / OnFailure exec pins. */
UCLASS()
class PYTHONCLI_API UPythonServiceCallAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FPythonCallResultSignature OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPythonCallResultSignature OnFailure;

	/** Call a function in a Python service's script and get its return value (deferred replies included).
	 JsonArgs: JSON array of arguments; Tensors are appended as numpy arrays. */
	UFUNCTION(BlueprintCallable, Category = "Python", meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "JsonArgs,Tensors", DisplayName = "Call Python Service"))
	static UPythonServiceCallAsyncAction* CallPythonService(UPythonService* Service, const FString& FunctionName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors);

	virtual void Activate() override;

private:
	void Finish(const FPythonCallResult& Result);

	TWeakObjectPtr<UPythonService> Service;
	FString FunctionName;
	FString JsonArgs;
	TArray<FPyTensor> Tensors;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPythonJobActionSignature, UPythonJob*, Job);

/** Blueprint: Start Python Job, with pins for each stage of the job. */
UCLASS()
class PYTHONCLI_API UPythonJobAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	/** Right away, with the job (keep it to Stop it). */
	UPROPERTY(BlueprintAssignable)
	FPythonJobActionSignature OnStarted;

	/** Job's Progress / Progress Message changed. */
	UPROPERTY(BlueprintAssignable)
	FPythonJobActionSignature OnProgress;

	/** job.metrics(): Job's Last Metrics Json / Get Metric. */
	UPROPERTY(BlueprintAssignable)
	FPythonJobActionSignature OnMetrics;

	/** The job returned (State Completed) or ended after Stop (State Stopped): Job's Result Json. */
	UPROPERTY(BlueprintAssignable)
	FPythonJobActionSignature OnCompleted;

	/** Job's Error / Traceback. */
	UPROPERTY(BlueprintAssignable)
	FPythonJobActionSignature OnFailed;

	/** Start a @ue.job in a Python service. ParamsJson: a JSON object of the job's parameters. Resume continues
	 the job's latest run. RunsDir: where run folders go (empty: Saved/PythonCLI/Runs). */
	UFUNCTION(BlueprintCallable, Category = "Python", meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "ParamsJson,RunsDir", DisplayName = "Start Python Job"))
	static UPythonJobAsyncAction* StartPythonJob(UPythonService* Service, const FString& JobName, const FString& ParamsJson, bool bResume, const FString& RunsDir);

	virtual void Activate() override;

private:
	UFUNCTION()
	void HandleProgress(UPythonJob* Job, float Progress, const FString& Message);
	UFUNCTION()
	void HandleMetrics(UPythonJob* Job, int32 Step, const FString& MetricsJson);
	UFUNCTION()
	void HandleFinished(UPythonJob* Job, const FString& ResultJson);
	UFUNCTION()
	void HandleFailed(UPythonJob* Job, const FString& Error, const FString& Traceback);
	void Done();

	TWeakObjectPtr<UPythonService> Service;
	FString JobName;
	FString ParamsJson;
	bool bResume = false;
	FString RunsDir;
	UPROPERTY()
	TObjectPtr<UPythonJob> Job;
};
