#include "PythonJob.h"
#include "PythonCLI.h"
#include "PythonService.h"
#include "PythonTypes.h"
#include "Dom/JsonObject.h"

void UPythonJob::Init(UPythonService* InService, const FString& InName, const FString& InId)
{
	Service = InService;
	JobName = InName;
	JobId = InId;
	State = EPythonJobState::Starting;
}

void UPythonJob::Stop()
{
	if (IsFinished())
	{
		return;
	}
	State = EPythonJobState::Stopping;
	if (UPythonService* S = Service.Get())
	{
		S->StopJob(JobId);
	}
}

float UPythonJob::GetMetric(const FString& Name, float Default) const
{
	double Value = Default;
	return LastMetrics.IsValid() && LastMetrics->TryGetNumberField(Name, Value) ? (float)Value : Default;
}

void UPythonJob::HandleStartReply(bool bOk, const TSharedPtr<FJsonValue>& InResult, const FString& InError, const FString& InTraceback)
{
	if (IsFinished())
	{
		return;
	}
	if (!bOk)
	{
		Fail(InError, InTraceback);
		return;
	}
	const TSharedPtr<FJsonObject>* Obj = nullptr;
	if (InResult.IsValid() && InResult->TryGetObject(Obj))
	{
		(*Obj)->TryGetStringField(TEXT("run_dir"), RunDir);
		(*Obj)->TryGetBoolField(TEXT("resumed"), bResumed);
	}
	if (State == EPythonJobState::Starting)
	{
		State = EPythonJobState::Running;
	}
}

void UPythonJob::HandleEvent(const TSharedPtr<FJsonObject>& Event)
{
	if (IsFinished())
	{
		return;
	}
	const FString Kind = Event->GetStringField(TEXT("kind"));
	if (Kind == TEXT("started"))
	{
		Event->TryGetStringField(TEXT("run_dir"), RunDir);
		Event->TryGetBoolField(TEXT("resumed"), bResumed);
		if (State == EPythonJobState::Starting)
		{
			State = EPythonJobState::Running;
		}
	}
	else if (Kind == TEXT("progress"))
	{
		double Value = Progress;
		Event->TryGetNumberField(TEXT("progress"), Value);
		Progress = (float)Value;
		Event->TryGetStringField(TEXT("message"), ProgressMessage);
		OnProgress.Broadcast(this, Progress, ProgressMessage);
	}
	else if (Kind == TEXT("metrics"))
	{
		const TSharedPtr<FJsonObject>* Metrics = nullptr;
		if (Event->TryGetObjectField(TEXT("metrics"), Metrics))
		{
			LastMetrics = *Metrics;
			LastMetricsJson = PythonWire::ToJsonString(MakeShared<FJsonValueObject>(*Metrics));
		}
		double Step = -1.0;
		Event->TryGetNumberField(TEXT("step"), Step);
		OnMetrics.Broadcast(this, (int32)Step, LastMetricsJson);
	}
	else if (Kind == TEXT("completed") || Kind == TEXT("stopped"))
	{
		Result = Event->TryGetField(TEXT("result"));
		ResultJson = PythonWire::ToJsonString(Result);
		State = Kind == TEXT("completed") ? EPythonJobState::Completed : EPythonJobState::Stopped;
		if (State == EPythonJobState::Completed)
		{
			Progress = 1.f;
		}
		UPythonService* S = Service.Get();
		if (S)
		{
			S->Jobs.Remove(JobId);
		}
		OnFinished.Broadcast(this, ResultJson);
		if (OnFinishedNative)
		{
			OnFinishedNative(this);
		}
	}
	else if (Kind == TEXT("failed"))
	{
		FString InError, InTraceback;
		Event->TryGetStringField(TEXT("error"), InError);
		Event->TryGetStringField(TEXT("traceback"), InTraceback);
		Fail(InError, InTraceback);
	}
}

void UPythonJob::Fail(const FString& InError, const FString& InTraceback)
{
	if (IsFinished())
	{
		return;
	}
	State = EPythonJobState::Failed;
	Error = InError;
	Traceback = InTraceback;
	UE_LOG(LogPythonCLI, Warning, TEXT("Python job '%s' failed: %s"), *JobName, *InError);
	if (UPythonService* S = Service.Get())
	{
		S->Jobs.Remove(JobId);
	}
	OnFailed.Broadcast(this, Error, Traceback);
	if (OnFinishedNative)
	{
		OnFinishedNative(this);
	}
}
