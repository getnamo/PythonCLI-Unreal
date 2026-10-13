#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "PythonJob.generated.h"

class FJsonObject;
class UPythonService;

UENUM(BlueprintType)
enum class EPythonJobState : uint8
{
	Starting,
	Running,
	/** Stop was requested; the job is finishing its current step. */
	Stopping,
	Completed,
	/** Ended early by Stop (or its script stopping). The result is whatever the job returned. */
	Stopped,
	Failed,
};

class UPythonJob;
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPythonJobProgressSignature, UPythonJob*, Job, float, Progress, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPythonJobMetricsSignature, UPythonJob*, Job, int32, Step, const FString&, MetricsJson);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPythonJobFinishedSignature, UPythonJob*, Job, const FString&, ResultJson);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPythonJobFailedSignature, UPythonJob*, Job, const FString&, Error, const FString&, Traceback);

/**
 * A job running in a Python service: a @ue.job function on its own thread (training, evaluation, data
 * processing), with progress, metrics, a run folder and a clean stop. Created by UPythonService::StartJob.
 */
UCLASS(BlueprintType)
class PYTHONCLI_API UPythonJob : public UObject
{
	GENERATED_BODY()
public:
	/** The @ue.job name. */
	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	FString JobName;

	/** Unique per start. */
	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	FString JobId;

	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	EPythonJobState State = EPythonJobState::Starting;

	/** 0..1 */
	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	float Progress = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	FString ProgressMessage;

	/** The run folder (config.json, metrics.jsonl, state.json, result.json, and whatever the job writes). */
	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	FString RunDir;

	/** Continued the latest run of this job (Start Job with Resume). */
	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	bool bResumed = false;

	/** The latest job.metrics() values as a JSON object. */
	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	FString LastMetricsJson;

	/** What the job function returned (Completed or Stopped), as JSON. */
	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	FString ResultJson;

	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	FString Error;

	UPROPERTY(BlueprintReadOnly, Category = "Python Job")
	FString Traceback;

	UPROPERTY(BlueprintAssignable, Category = "Python Job")
	FPythonJobProgressSignature OnProgress;

	/** job.metrics(step=..., **values): Step is -1 when not given. */
	UPROPERTY(BlueprintAssignable, Category = "Python Job")
	FPythonJobMetricsSignature OnMetrics;

	/** The job returned (State Completed), or ended early after Stop (State Stopped). */
	UPROPERTY(BlueprintAssignable, Category = "Python Job")
	FPythonJobFinishedSignature OnFinished;

	UPROPERTY(BlueprintAssignable, Category = "Python Job")
	FPythonJobFailedSignature OnFailed;

	/** Ask the job to end (job.should_stop becomes true; job.range / job.track end at the next step). */
	UFUNCTION(BlueprintCallable, Category = "Python Job")
	void Stop();

	UFUNCTION(BlueprintPure, Category = "Python Job")
	bool IsFinished() const { return State == EPythonJobState::Completed || State == EPythonJobState::Stopped || State == EPythonJobState::Failed; }

	/** A value from the latest metrics, or Default. */
	UFUNCTION(BlueprintPure, Category = "Python Job")
	float GetMetric(const FString& Name, float Default = 0.f) const;

	UFUNCTION(BlueprintPure, Category = "Python Job")
	UPythonService* GetService() const { return Service.Get(); }

	/** C++: the latest metrics and the parsed result. */
	TSharedPtr<FJsonObject> LastMetrics;
	TSharedPtr<class FJsonValue> Result;

	/** Native callbacks (C++), in addition to the dynamic delegates. */
	TFunction<void(UPythonJob*)> OnFinishedNative;

	/** UPythonService: the start call's reply and the job's events. */
	void Init(UPythonService* InService, const FString& InName, const FString& InId);
	void HandleStartReply(bool bOk, const TSharedPtr<FJsonValue>& InResult, const FString& InError, const FString& InTraceback);
	void HandleEvent(const TSharedPtr<FJsonObject>& Event);
	void Fail(const FString& InError, const FString& InTraceback = FString());

private:
	TWeakObjectPtr<UPythonService> Service;
};
