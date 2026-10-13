#include "PythonServiceAsyncActions.h"
#include "PythonJob.h"
#include "PythonService.h"

UPythonServiceCallAsyncAction* UPythonServiceCallAsyncAction::CallPythonService(UPythonService* InService, const FString& InFunctionName, const FString& InJsonArgs, const TArray<FPyTensor>& InTensors)
{
	UPythonServiceCallAsyncAction* Action = NewObject<UPythonServiceCallAsyncAction>();
	Action->Service = InService;
	Action->FunctionName = InFunctionName;
	Action->JsonArgs = InJsonArgs;
	Action->Tensors = InTensors;
	Action->AddToRoot(); //Services outlive worlds: kept alive until the reply instead of by a game instance
	return Action;
}

void UPythonServiceCallAsyncAction::Activate()
{
	UPythonService* Target = Service.Get();
	if (!Target)
	{
		FPythonCallResult R;
		R.Error = TEXT("Call Python Service: no service");
		Finish(R);
		return;
	}
	TWeakObjectPtr<UPythonServiceCallAsyncAction> Weak(this);
	Target->CallFunction(FunctionName, JsonArgs, Tensors, [Weak](const FPythonCallResult& Result)
	{
		if (UPythonServiceCallAsyncAction* Self = Weak.Get())
		{
			Self->Finish(Result);
		}
	});
}

void UPythonServiceCallAsyncAction::Finish(const FPythonCallResult& Result)
{
	(Result.bSuccess ? OnSuccess : OnFailure).Broadcast(Result);
	RemoveFromRoot();
	SetReadyToDestroy();
}

UPythonJobAsyncAction* UPythonJobAsyncAction::StartPythonJob(UPythonService* InService, const FString& InJobName, const FString& InParamsJson, bool bInResume, const FString& InRunsDir)
{
	UPythonJobAsyncAction* Action = NewObject<UPythonJobAsyncAction>();
	Action->Service = InService;
	Action->JobName = InJobName;
	Action->ParamsJson = InParamsJson;
	Action->bResume = bInResume;
	Action->RunsDir = InRunsDir;
	Action->AddToRoot();
	return Action;
}

void UPythonJobAsyncAction::Activate()
{
	UPythonService* Target = Service.Get();
	if (!Target)
	{
		Job = NewObject<UPythonJob>(this);
		Job->Init(nullptr, JobName, FString());
		OnStarted.Broadcast(Job);
		Job->Fail(TEXT("Start Python Job: no service"));
		OnFailed.Broadcast(Job);
		Done();
		return;
	}
	Job = Target->StartJob(JobName, ParamsJson, bResume, RunsDir);
	Job->OnProgress.AddDynamic(this, &UPythonJobAsyncAction::HandleProgress);
	Job->OnMetrics.AddDynamic(this, &UPythonJobAsyncAction::HandleMetrics);
	Job->OnFinished.AddDynamic(this, &UPythonJobAsyncAction::HandleFinished);
	Job->OnFailed.AddDynamic(this, &UPythonJobAsyncAction::HandleFailed);
	OnStarted.Broadcast(Job);
	if (Job->State == EPythonJobState::Failed) //Bad parameters fail at once
	{
		OnFailed.Broadcast(Job);
		Done();
	}
}

void UPythonJobAsyncAction::HandleProgress(UPythonJob* InJob, float, const FString&)
{
	OnProgress.Broadcast(InJob);
}

void UPythonJobAsyncAction::HandleMetrics(UPythonJob* InJob, int32, const FString&)
{
	OnMetrics.Broadcast(InJob);
}

void UPythonJobAsyncAction::HandleFinished(UPythonJob* InJob, const FString&)
{
	OnCompleted.Broadcast(InJob);
	Done();
}

void UPythonJobAsyncAction::HandleFailed(UPythonJob* InJob, const FString&, const FString&)
{
	OnFailed.Broadcast(InJob);
	Done();
}

void UPythonJobAsyncAction::Done()
{
	if (IsRooted())
	{
		RemoveFromRoot();
	}
	SetReadyToDestroy();
}
