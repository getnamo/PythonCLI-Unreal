#include "PythonService.h"
#include "PythonCLI.h"
#include "PythonCLISettings.h"
#include "PythonEnvironment.h"
#include "PythonJob.h"
#include "PythonProcess.h"
#include "PythonSubsystem.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

UPythonService::UPythonService()
{
}

void UPythonService::Configure(const FString& InName, const FPythonScriptParams& InScript, EPythonServiceProcess InProcess)
{
	ServiceName = InName;
	ScriptParams = InScript;
	ProcessMode = InProcess;
	OwnerId = TEXT("svc:") + InName;
}

FString UPythonService::ResolveScriptPath() const
{
	return FPythonEnvironment::ResolveScriptPath(ScriptParams);
}

int32 UPythonService::GetPythonProcessId() const
{
	return Process.IsValid() ? Process->GetPid() : 0;
}

TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> UPythonService::GetProcess()
{
	return EnsureProcess() ? Process : nullptr;
}

bool UPythonService::EnsureProcess()
{
	if (Process.IsValid())
	{
		return true;
	}
	UPythonSubsystem* Subsystem = UPythonSubsystem::Get();
	if (!Subsystem || OwnerId.IsEmpty())
	{
		return false;
	}
	Process = ProcessMode == EPythonServiceProcess::Shared ? Subsystem->GetSharedProcess() : Subsystem->CreateDedicatedProcess(TEXT("svc-") + ServiceName);
	if (!Process.IsValid())
	{
		return false;
	}
	TWeakObjectPtr<UPythonService> Weak(this);
	Process->AddOwner(OwnerId,
		[Weak](const FPythonFrame& Frame)
		{
			if (UPythonService* Self = Weak.Get())
			{
				Self->HandleFrame(Frame);
			}
		},
		[Weak](int32 Code, const FString& Message)
		{
			if (UPythonService* Self = Weak.Get())
			{
				Self->HandleProcessExit(Code, Message);
			}
		});
	return true;
}

void UPythonService::SendControl(const FString& Command)
{
	if (!EnsureProcess())
	{
		return;
	}
	TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
	Header->SetStringField(TEXT("cmd"), Command);
	Header->SetStringField(TEXT("owner"), OwnerId);
	Header->SetStringField(TEXT("script"), ScriptParams.Script);
	if (Command == TEXT("start") || Command == TEXT("reload"))
	{
		Header->SetStringField(TEXT("path"), ResolveScriptPath());
		TArray<TSharedPtr<FJsonValue>> Args;
		for (const FString& Arg : ScriptParams.Args)
		{
			Args.Add(MakeShared<FJsonValueString>(Arg));
		}
		Header->SetArrayField(TEXT("args"), Args);
	}
	Process->Send(PythonWire::EncodeFrame(PythonWire::Control, Header));
}

void UPythonService::Start()
{
	if (bScriptRunning || bScriptStarting)
	{
		return;
	}
	const FString Path = ResolveScriptPath();
	if (ScriptParams.Script.IsEmpty() || !FPaths::FileExists(Path))
	{
		const FString Message = FString::Printf(TEXT("Python service '%s': script not found: %s"), *ServiceName, *Path);
		UE_LOG(LogPythonCLI, Error, TEXT("%s"), *Message);
		OnScriptError.Broadcast(Message, FString());
		FailPendingCalls(Message);
		return;
	}
	if (!EnsureProcess())
	{
		FailPendingCalls(TEXT("Python is not available"));
		return;
	}
	bScriptStarting = true;
	SendControl(TEXT("start"));
}

void UPythonService::Stop()
{
	if (Process.IsValid() && (bScriptRunning || bScriptStarting))
	{
		SendControl(TEXT("stop"));
	}
	bScriptStarting = false;
}

void UPythonService::Reload()
{
	if (!bScriptRunning && !bScriptStarting)
	{
		Start();
		return;
	}
	bScriptStarting = true;
	SendControl(TEXT("reload"));
}

void UPythonService::EmitEvent(const FString& EventName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors)
{
	Start();
	SendEvent(EventName, JsonArgs, Tensors, 0);
}

void UPythonService::CallFunction(const FString& FunctionName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors, TFunction<void(const FPythonCallResult&)> OnResult)
{
	const int32 Id = ++NextCallId;
	const bool bWantsReply = (bool)OnResult;
	if (bWantsReply)
	{
		PendingCalls.Add(Id, MoveTemp(OnResult));
	}
	Start(); //The call queues behind the start in Python
	if (bWantsReply && !PendingCalls.Contains(Id))
	{
		return; //The script couldn't start: already answered
	}
	SendEvent(FunctionName, JsonArgs, Tensors, bWantsReply ? Id : 0);
}

void UPythonService::SendEvent(const FString& EventName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors, int32 CallId)
{
	auto Fail = [this, CallId, &EventName](const FString& Error)
	{
		UE_LOG(LogPythonCLI, Error, TEXT("Python service '%s', %s: %s"), *ServiceName, *EventName, *Error);
		TFunction<void(const FPythonCallResult&)> Callback;
		if (CallId && PendingCalls.RemoveAndCopyValue(CallId, Callback) && Callback)
		{
			FPythonCallResult R;
			R.Error = Error;
			Callback(R);
		}
	};
	TArray<TSharedPtr<FJsonValue>> Args;
	TArray<uint8> Table;
	FString Error;
	if (!PythonWire::PackArgs(JsonArgs, Tensors, Args, Table, Error))
	{
		Fail(Error);
		return;
	}
	if (!EnsureProcess())
	{
		Fail(TEXT("Python is not available"));
		return;
	}
	TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
	Header->SetStringField(TEXT("owner"), OwnerId);
	Header->SetStringField(TEXT("script"), ScriptParams.Script);
	Header->SetStringField(TEXT("name"), EventName);
	Header->SetArrayField(TEXT("args"), Args);
	if (CallId)
	{
		Header->SetNumberField(TEXT("id"), CallId);
	}
	Process->Send(PythonWire::EncodeFrame(PythonWire::Event, Header, Table));
}

void UPythonService::HandleFrame(const FPythonFrame& Frame)
{
	const TSharedPtr<FJsonObject>& H = Frame.Header;
	FString Script;
	H->TryGetStringField(TEXT("script"), Script);
	const bool bOurScript = Script.IsEmpty() || Script == ScriptParams.Script;
	switch (Frame.Type)
	{
	case PythonWire::Status:
	{
		if (!bOurScript)
		{
			return;
		}
		const FString Status = H->GetStringField(TEXT("status"));
		if (Status == TEXT("started"))
		{
			bScriptStarting = false;
			bScriptRunning = true;
			OnScriptStarted.Broadcast(Script);
		}
		else if (Status == TEXT("stopped"))
		{
			bScriptRunning = false;
			if (!bScriptStarting)
			{
				OnScriptStopped.Broadcast(Script);
			}
		}
		else if (Status == TEXT("error"))
		{
			bScriptStarting = false;
			bScriptRunning = false;
			const FString Error = H->GetStringField(TEXT("error"));
			FString Traceback;
			H->TryGetStringField(TEXT("traceback"), Traceback);
			UE_LOG(LogPythonCLI, Error, TEXT("[python %s] %s"), *Script, *Error);
			OnScriptError.Broadcast(Error, Traceback);
			FailPendingCalls(FString::Printf(TEXT("Python service '%s' failed to start: %s"), *ServiceName, *Error));
		}
		return;
	}
	case PythonWire::Log:
	{
		const FString Level = H->GetStringField(TEXT("level"));
		const FString Message = H->GetStringField(TEXT("msg"));
		if (bLogScriptOutput)
		{
			PythonWire::LogScriptMessage(Script, Level, Message);
		}
		OnLog.Broadcast(Message, Level);
		return;
	}
	case PythonWire::Event:
	{
		FPythonEventData Data = PythonWire::ParseEvent(H, Frame.Binary);
		if (Data.Name == TEXT("_job"))
		{
			const TSharedPtr<FJsonObject>* Event = nullptr;
			FString Id;
			if (Data.Args.Num() > 0 && Data.Args[0]->TryGetObject(Event) && (*Event)->TryGetStringField(TEXT("id"), Id))
			{
				if (TObjectPtr<UPythonJob>* Job = Jobs.Find(Id))
				{
					UPythonJob* J = *Job; //Kept alive here: finishing removes it from the map
					J->HandleEvent(*Event);
				}
			}
			return;
		}
		if (FPythonServiceNativeEvent* Native = NativeEvents.Find(Data.Name))
		{
			Native->Broadcast(Data);
		}
		OnEvent.Broadcast(Data.Name, Data.JsonArgs, Data);
		return;
	}
	case PythonWire::Reply:
	{
		const int32 Id = (int32)H->GetNumberField(TEXT("id"));
		TFunction<void(const FPythonCallResult&)> Callback;
		if (PendingCalls.RemoveAndCopyValue(Id, Callback) && Callback)
		{
			Callback(PythonWire::ParseReply(H, Frame.Binary));
		}
		return;
	}
	default:
		return;
	}
}

void UPythonService::HandleProcessExit(int32 Code, const FString& Message)
{
	const bool bHadScript = bScriptRunning || bScriptStarting;
	bScriptRunning = false;
	bScriptStarting = false;
	const FString Reason = Message.IsEmpty() ? TEXT("the Python process exited") : Message;
	FailPendingCalls(Reason);
	TArray<TObjectPtr<UPythonJob>> Running;
	Jobs.GenerateValueArray(Running);
	for (UPythonJob* Job : Running)
	{
		Job->Fail(Reason);
	}
	if (bHadScript && !Message.IsEmpty())
	{
		OnScriptError.Broadcast(Message, FString());
	}
}

void UPythonService::FailPendingCalls(const FString& Error)
{
	TMap<int32, TFunction<void(const FPythonCallResult&)>> Calls = MoveTemp(PendingCalls);
	PendingCalls.Reset();
	for (TPair<int32, TFunction<void(const FPythonCallResult&)>>& Pair : Calls)
	{
		if (Pair.Value)
		{
			FPythonCallResult R;
			R.Error = Error;
			Pair.Value(R);
		}
	}
}

UPythonJob* UPythonService::StartJob(const FString& JobName, const FString& ParamsJson, bool bResume, const FString& RunsDir)
{
	UPythonJob* Job = NewObject<UPythonJob>(this);
	const FString Id = FString::Printf(TEXT("%s-%d"), *JobName, ++NextJobId);
	Job->Init(this, JobName, Id);
	Jobs.Add(Id, Job);

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	if (!ParamsJson.TrimStartAndEnd().IsEmpty() && !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ParamsJson), Params))
	{
		Job->Fail(FString::Printf(TEXT("job parameters are not a JSON object: %s"), *ParamsJson));
		return Job;
	}
	TSharedRef<FJsonObject> Options = MakeShared<FJsonObject>();
	Options->SetBoolField(TEXT("resume"), bResume);
	if (!RunsDir.IsEmpty())
	{
		Options->SetStringField(TEXT("runs_dir"), FPaths::ConvertRelativePathToFull(FPaths::IsRelative(RunsDir) ? FPaths::ProjectDir() / RunsDir : RunsDir));
	}
	else
	{
		Options->SetStringField(TEXT("runs_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PythonCLI/Runs")));
	}
	const FString Args = PythonWire::ToJsonString(TArray<TSharedPtr<FJsonValue>>{
		MakeShared<FJsonValueString>(Id), MakeShared<FJsonValueString>(JobName),
		MakeShared<FJsonValueObject>(Params), MakeShared<FJsonValueObject>(Options) });
	TWeakObjectPtr<UPythonJob> WeakJob(Job);
	CallFunction(TEXT("_job_start"), Args, {}, [WeakJob](const FPythonCallResult& R)
	{
		if (UPythonJob* J = WeakJob.Get())
		{
			J->HandleStartReply(R.bSuccess, R.Result, R.Error, R.Traceback);
		}
	});
	return Job;
}

void UPythonService::StopJob(const FString& Id)
{
	if (Jobs.Contains(Id))
	{
		CallFunction(TEXT("_job_stop"), PythonWire::ToJsonString(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueString>(Id) }), {}, nullptr);
	}
}

TArray<UPythonJob*> UPythonService::GetRunningJobs() const
{
	TArray<UPythonJob*> Out;
	for (const TPair<FString, TObjectPtr<UPythonJob>>& Pair : Jobs)
	{
		if (Pair.Value && !Pair.Value->IsFinished())
		{
			Out.Add(Pair.Value);
		}
	}
	return Out;
}

void UPythonService::ListJobs(TFunction<void(const FPythonCallResult&)> OnResult)
{
	CallFunction(TEXT("_job_list"), TEXT("[]"), {}, MoveTemp(OnResult));
}

void UPythonService::Shutdown()
{
	const FString Reason = FString::Printf(TEXT("Python service '%s' was shut down"), *ServiceName);
	FailPendingCalls(Reason);
	TArray<TObjectPtr<UPythonJob>> Running;
	Jobs.GenerateValueArray(Running);
	for (UPythonJob* Job : Running)
	{
		Job->Fail(Reason);
	}
	if (Process.IsValid())
	{
		Process->RemoveOwner(OwnerId); //Stops the script (on_stop runs, its jobs are stopped)
		if (ProcessMode == EPythonServiceProcess::Dedicated)
		{
			Process->Stop(GetDefault<UPythonCLISettings>()->ShutdownGraceSeconds);
		}
		Process.Reset();
	}
	bScriptRunning = false;
	bScriptStarting = false;
}

void UPythonService::BeginDestroy()
{
	if (Process.IsValid())
	{
		Process->RemoveOwner(OwnerId);
		if (ProcessMode == EPythonServiceProcess::Dedicated)
		{
			Process->Stop(0.2f);
		}
		Process.Reset();
	}
	Super::BeginDestroy();
}
