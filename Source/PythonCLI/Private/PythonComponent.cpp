#include "PythonComponent.h"
#include "PythonEnvironment.h"
#include "PythonProcess.h"
#include "PythonCLI.h"
#include "PythonCLISettings.h"
#include "PythonSubsystem.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"

namespace PythonComponentPrivate
{
	static uint32 NextOwner = 0;
}

UPythonComponent::UPythonComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	OwnerId = FString::Printf(TEXT("pc%u"), ++PythonComponentPrivate::NextOwner);
}

int32 UPythonComponent::GetPythonProcessId() const
{
	return Process.IsValid() ? Process->GetPid() : 0;
}

TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> UPythonComponent::GetProcess()
{
	return EnsureProcess() ? Process : nullptr;
}

FString UPythonComponent::ResolveScriptPath() const
{
	//The project's scripts first, then folders other plugins registered, then this plugin's Content/Python.
	//"my_model" also finds my_model.py (TensorFlow-Unreal 1.x named modules without the extension).
	TArray<FString> Roots = { FPaths::ProjectDir() / ScriptParams.ScriptRoot };
	Roots.Append(FPythonEnvironment::GetScriptDirectories());
	Roots.Add(FPythonEnvironment::PluginPythonDir());
	const bool bNoExtension = FPaths::GetExtension(ScriptParams.Script).IsEmpty();
	for (const FString& Root : Roots)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(Root / ScriptParams.Script);
		if (FPaths::FileExists(Path))
		{
			return Path;
		}
		if (bNoExtension && FPaths::FileExists(Path + TEXT(".py")))
		{
			return Path + TEXT(".py");
		}
	}
	return FPaths::ConvertRelativePathToFull(Roots[0] / ScriptParams.Script);
}

void UPythonComponent::BeginPlay()
{
	Super::BeginPlay();
	if (bStartScriptOnBeginPlay && !ScriptParams.Script.IsEmpty())
	{
		StartScript();
	}
}

void UPythonComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	FailPendingCalls(TEXT("the Python component ended"));
	ReleaseProcess();
	bScriptRunning = false;
	bScriptStarting = false;
	Super::EndPlay(EndPlayReason);
}

bool UPythonComponent::EnsureProcess()
{
	if (Process.IsValid())
	{
		return true;
	}
	UPythonSubsystem* Subsystem = UPythonSubsystem::Get();
	if (!Subsystem)
	{
		return false;
	}
	Process = bUseSharedProcess ? Subsystem->GetSharedProcess() : Subsystem->CreateDedicatedProcess(OwnerId);
	TWeakObjectPtr<UPythonComponent> Weak(this);
	Process->AddOwner(OwnerId,
		[Weak](const FPythonFrame& Frame)
		{
			if (UPythonComponent* Self = Weak.Get())
			{
				Self->HandleFrame(Frame);
			}
		},
		[Weak](int32 Code, const FString& Message)
		{
			if (UPythonComponent* Self = Weak.Get())
			{
				Self->HandleProcessExit(Code, Message);
			}
		});
	return true;
}

void UPythonComponent::ReleaseProcess()
{
	if (!Process.IsValid())
	{
		return;
	}
	Process->RemoveOwner(OwnerId); //Stops this owner's scripts (on_stop runs) in the shared process
	if (!bUseSharedProcess)
	{
		Process->Stop(GetDefault<UPythonCLISettings>()->ShutdownGraceSeconds);
	}
	Process.Reset();
}

void UPythonComponent::SendControl(const FString& Command)
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

void UPythonComponent::StartScript()
{
	if (ScriptParams.Script.IsEmpty())
	{
		OnScriptError.Broadcast(TEXT("No script set (ScriptParams.Script)"), FString());
		return;
	}
	const FString Path = ResolveScriptPath();
	if (!FPaths::FileExists(Path))
	{
		const FString Message = FString::Printf(TEXT("Python script not found: %s"), *Path);
		UE_LOG(LogPythonCLI, Error, TEXT("%s"), *Message);
		OnScriptError.Broadcast(Message, FString());
		return;
	}
	bScriptStarting = true;
	SendControl(bScriptRunning ? TEXT("reload") : TEXT("start"));
}

void UPythonComponent::StopScript()
{
	if (Process.IsValid() && (bScriptRunning || bScriptStarting))
	{
		SendControl(TEXT("stop"));
	}
	bScriptStarting = false;
}

void UPythonComponent::ReloadScript()
{
	bScriptStarting = true;
	SendControl(TEXT("reload"));
}

void UPythonComponent::EmitEvent(const FString& EventName, const FString& JsonArgs)
{
	SendEvent(EventName, JsonArgs, {}, 0);
}

void UPythonComponent::EmitEventWithTensors(const FString& EventName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors)
{
	SendEvent(EventName, JsonArgs, Tensors, 0);
}

void UPythonComponent::CallFunction(const FString& FunctionName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors, TFunction<void(const FPythonCallResult&)> OnResult)
{
	const int32 Id = ++NextCallId;
	if (OnResult)
	{
		PendingCalls.Add(Id, MoveTemp(OnResult));
	}
	SendEvent(FunctionName, JsonArgs, Tensors, Id);
}

void UPythonComponent::SendEvent(const FString& EventName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors, int32 CallId)
{
	auto Fail = [this, CallId](const FString& Error)
	{
		UE_LOG(LogPythonCLI, Error, TEXT("%s"), *Error);
		TFunction<void(const FPythonCallResult&)> Callback;
		if (PendingCalls.RemoveAndCopyValue(CallId, Callback) && Callback)
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
		if (CallId)
		{
			Fail(FString::Printf(TEXT("%s: %s"), *EventName, *Error));
		}
		else
		{
			UE_LOG(LogPythonCLI, Error, TEXT("%s: %s"), *EventName, *Error);
		}
		return;
	}
	if (!EnsureProcess())
	{
		if (CallId)
		{
			Fail(TEXT("Python runtime is not available"));
		}
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

void UPythonComponent::BindEventNative(const FString& EventName, TFunction<void(const FPythonEventData&)> Handler)
{
	NativeHandlers.Add(EventName, MoveTemp(Handler));
}

void UPythonComponent::UnbindEventNative(const FString& EventName)
{
	NativeHandlers.Remove(EventName);
}

void UPythonComponent::HandleFrame(const FPythonFrame& Frame)
{
	const TSharedPtr<FJsonObject>& H = Frame.Header;
	FString Script;
	H->TryGetStringField(TEXT("script"), Script);
	const bool bOurScript = Script.IsEmpty() || Script == ScriptParams.Script;

	switch (Frame.Type)
	{
	case PythonWire::Status:
	{
		const FString Status = H->GetStringField(TEXT("status"));
		if (!bOurScript)
		{
			return;
		}
		if (Status == TEXT("started"))
		{
			bScriptStarting = false;
			bScriptRunning = true;
			HandleScriptStarted(Script);
			OnScriptStarted.Broadcast(Script);
		}
		else if (Status == TEXT("stopped"))
		{
			//A reload reports the old instance stopped, then the new one started
			bScriptRunning = false;
			HandleScriptStopped(Script);
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
		}
		return;
	}
	case PythonWire::Log:
	{
		const FString Level = H->GetStringField(TEXT("level"));
		const FString Message = H->GetStringField(TEXT("msg"));
		if (bLogScriptOutput)
		{
			TArray<FString> Lines;
			Message.ParseIntoArrayLines(Lines, false);
			for (const FString& Line : Lines)
			{
				if (Level == TEXT("error")) { UE_LOG(LogPythonCLI, Error, TEXT("[python %s] %s"), *Script, *Line); }
				else if (Level == TEXT("warning")) { UE_LOG(LogPythonCLI, Warning, TEXT("[python %s] %s"), *Script, *Line); }
				else { UE_LOG(LogPythonCLI, Log, TEXT("[python %s] %s"), *Script, *Line); }
			}
		}
		OnLog.Broadcast(Message, Level);
		return;
	}
	case PythonWire::Event:
	{
		FPythonEventData Data;
		Data.Name = H->GetStringField(TEXT("name"));
		Data.Script = Script;
		const TSharedPtr<FJsonValue> Args = H->TryGetField(TEXT("args"));
		if (Args.IsValid() && Args->Type == EJson::Array)
		{
			Data.Args = Args->AsArray();
		}
		Data.JsonArgs = PythonWire::ToJsonString(Data.Args);
		Data.Tensors = PythonWire::UnpackTensors(Args, Frame.Binary);
		if (HandleScriptEvent(Data))
		{
			return;
		}
		if (TFunction<void(const FPythonEventData&)>* Native = NativeHandlers.Find(Data.Name))
		{
			(*Native)(Data);
		}
		OnEvent.Broadcast(Data.Name, Data.JsonArgs, Data);
		return;
	}
	case PythonWire::Reply:
	{
		const int32 Id = (int32)H->GetNumberField(TEXT("id"));
		TFunction<void(const FPythonCallResult&)> Callback;
		if (!PendingCalls.RemoveAndCopyValue(Id, Callback) || !Callback)
		{
			return;
		}
		FPythonCallResult R;
		R.bSuccess = H->GetBoolField(TEXT("ok"));
		if (R.bSuccess)
		{
			R.Result = H->TryGetField(TEXT("result"));
			R.JsonResult = PythonWire::ToJsonString(R.Result);
			R.Tensors = PythonWire::UnpackTensors(R.Result, Frame.Binary);
		}
		else
		{
			H->TryGetStringField(TEXT("error"), R.Error);
			H->TryGetStringField(TEXT("traceback"), R.Traceback);
		}
		Callback(R);
		return;
	}
	default:
		return;
	}
}

void UPythonComponent::HandleProcessExit(int32 Code, const FString& Message)
{
	const bool bHadScript = bScriptRunning || bScriptStarting;
	bScriptRunning = false;
	bScriptStarting = false;
	FailPendingCalls(Message.IsEmpty() ? TEXT("the Python process exited") : Message);
	//Keep the process object: it restarts on the next StartScript/EmitEvent
	if (bHadScript && !Message.IsEmpty())
	{
		OnScriptError.Broadcast(Message, FString());
	}
}

void UPythonComponent::FailPendingCalls(const FString& Error)
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
