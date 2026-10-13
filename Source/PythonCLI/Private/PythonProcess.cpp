#include "PythonProcess.h"
#include "PythonEnvironment.h"
#include "PythonCLI.h"
#include "PythonCLISettings.h"
#include "PythonStream.h"
#include "Async/Async.h"
#include "CLIFrameCodec.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SubProcess.h"
#include <atomic>

struct FPythonProcess::FReaderState
{
	FCLIFrameCodec Decoder{ FCLIFrameMagic('P', 'U', 'E', 1) };
	std::atomic<bool> bExiting{ false };
};

namespace PythonProcessPrivate
{
	static void LogLines(ELogVerbosity::Type Verbosity, const FString& Prefix, const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		for (const FString& Line : Lines)
		{
			if (Line.IsEmpty())
			{
				continue;
			}
			switch (Verbosity)
			{
			case ELogVerbosity::Error: UE_LOG(LogPythonCLI, Error, TEXT("%s%s"), *Prefix, *Line); break;
			case ELogVerbosity::Warning: UE_LOG(LogPythonCLI, Warning, TEXT("%s%s"), *Prefix, *Line); break;
			default: UE_LOG(LogPythonCLI, Log, TEXT("%s%s"), *Prefix, *Line); break;
			}
		}
	}

	static FString Utf8ToString(const uint8* Data, int32 Num)
	{
		FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data), Num);
		return FString(Converted.Length(), Converted.Get());
	}
}

// ------------------------------------------------------------------------------------------ FPythonChannel

struct FPythonChannel::FWaiter
{
	FEvent* Event = FPlatformProcess::GetSynchEventFromPool(true);
	bool bDone = false; //Guarded by the channel's lock, like Result
	FPythonCallResult Result;

	~FWaiter() { FPlatformProcess::ReturnSynchEventToPool(Event); }
};

FPythonChannel::~FPythonChannel()
{
	Detach(TEXT("the Python process is gone"));
}

bool FPythonChannel::Send(const TArray<uint8>& Frame)
{
	FScopeLock L(&Lock);
	if (!Handler.IsValid())
	{
		return false;
	}
	Handler->SendInput(Frame); //Async writes: a lock-free queue for the writer thread
	return true;
}

bool FPythonChannel::IsRunning() const
{
	FScopeLock L(&Lock);
	return Handler.IsValid();
}

void FPythonChannel::AddStream(const TSharedRef<FPythonStream, ESPMode::ThreadSafe>& Stream)
{
	FScopeLock L(&Lock);
	Streams.Add(Stream->GetKey(), Stream);
}

void FPythonChannel::RemoveStream(const FString& Key)
{
	FScopeLock L(&Lock);
	Streams.Remove(Key);
}

TArray<TSharedRef<FPythonStream, ESPMode::ThreadSafe>> FPythonChannel::GetStreams() const
{
	TArray<TSharedRef<FPythonStream, ESPMode::ThreadSafe>> Out;
	FScopeLock L(&Lock);
	for (const TPair<FString, TWeakPtr<FPythonStream, ESPMode::ThreadSafe>>& Pair : Streams)
	{
		if (TSharedPtr<FPythonStream, ESPMode::ThreadSafe> S = Pair.Value.Pin())
		{
			Out.Add(S.ToSharedRef());
		}
	}
	return Out;
}

int64 FPythonChannel::BeginWait()
{
	FScopeLock L(&Lock);
	const int64 Id = ++NextWaitId;
	Waiters.Add(Id, MakeShared<FWaiter, ESPMode::ThreadSafe>());
	return Id;
}

void FPythonChannel::CancelWait(int64 Id)
{
	FScopeLock L(&Lock);
	Waiters.Remove(Id);
}

bool FPythonChannel::Wait(int64 Id, double TimeoutSeconds, FPythonCallResult& Out)
{
	TSharedPtr<FWaiter, ESPMode::ThreadSafe> W;
	{
		FScopeLock L(&Lock);
		W = Waiters.FindRef(Id);
	}
	if (!W.IsValid())
	{
		Out = FPythonCallResult();
		Out.Error = TEXT("unknown step");
		return false;
	}
	W->Event->Wait(FTimespan::FromSeconds(FMath::Max(TimeoutSeconds, 0.0)));
	FScopeLock L(&Lock);
	Waiters.Remove(Id);
	if (!W->bDone)
	{
		Out = FPythonCallResult();
		Out.Error = FString::Printf(TEXT("no result from Python within %.1f s"), TimeoutSeconds);
		return false;
	}
	Out = MoveTemp(W->Result);
	return Out.bSuccess;
}

void FPythonChannel::Attach(TSharedPtr<FSubProcessHandler> InHandler, uint32 InGeneration)
{
	FScopeLock L(&Lock);
	Handler = MoveTemp(InHandler);
	Generation = InGeneration;
}

void FPythonChannel::Detach(const FString& Reason)
{
	TArray<TSharedPtr<FPythonStream, ESPMode::ThreadSafe>> Reset;
	{
		FScopeLock L(&Lock);
		Handler.Reset();
		Generation = 0;
		for (TPair<int64, TSharedPtr<FWaiter, ESPMode::ThreadSafe>>& Pair : Waiters)
		{
			if (!Pair.Value->bDone)
			{
				Pair.Value->bDone = true;
				Pair.Value->Result = FPythonCallResult();
				Pair.Value->Result.Error = Reason;
				Pair.Value->Event->Trigger();
			}
		}
		for (const TPair<FString, TWeakPtr<FPythonStream, ESPMode::ThreadSafe>>& Pair : Streams)
		{
			Reset.Add(Pair.Value.Pin());
		}
	}
	for (const TSharedPtr<FPythonStream, ESPMode::ThreadSafe>& S : Reset)
	{
		if (S.IsValid())
		{
			S->ResetSlots();
		}
	}
}

bool FPythonChannel::HandleOnPipeThread(uint32 FrameGeneration, uint8 Type, const TSharedPtr<FJsonObject>& H, const TArray<uint8>& Binary)
{
	if (Type == PythonWire::Ack)
	{
		TSharedPtr<FPythonStream, ESPMode::ThreadSafe> Stream;
		{
			FScopeLock L(&Lock);
			if (FrameGeneration != Generation)
			{
				return true; //From an earlier launch: its slots were reset when it ended
			}
			Stream = Streams.FindRef(H->GetStringField(TEXT("owner")) + TEXT("/") + H->GetStringField(TEXT("stream"))).Pin();
		}
		if (Stream.IsValid())
		{
			Stream->HandleAck((int32)H->GetNumberField(TEXT("slot")));
		}
		return true;
	}
	if (Type == PythonWire::Reply && H->HasField(TEXT("sid")))
	{
		FPythonCallResult R;
		R.bSuccess = H->GetBoolField(TEXT("ok"));
		if (R.bSuccess)
		{
			R.Result = H->TryGetField(TEXT("result"));
			R.JsonResult = PythonWire::ToJsonString(R.Result);
			R.Tensors = PythonWire::UnpackTensors(R.Result, Binary);
		}
		else
		{
			H->TryGetStringField(TEXT("error"), R.Error);
			H->TryGetStringField(TEXT("traceback"), R.Traceback);
		}
		FScopeLock L(&Lock);
		const TSharedPtr<FWaiter, ESPMode::ThreadSafe> W = Waiters.FindRef((int64)H->GetNumberField(TEXT("sid")));
		if (W.IsValid() && !W->bDone && FrameGeneration == Generation)
		{
			W->Result = MoveTemp(R);
			W->bDone = true;
			W->Event->Trigger();
		}
		return true; //A late reply (the step timed out) is dropped
	}
	return false;
}

// ------------------------------------------------------------------------------------------ FPythonProcess

FPythonProcess::FPythonProcess(const FString& InName, bool bInPreload)
	: Name(InName)
	, bPreload(bInPreload)
	, Channel(MakeShared<FPythonChannel, ESPMode::ThreadSafe>())
{
}

FPythonProcess::~FPythonProcess()
{
	++Generation;
	Channel->Detach(TEXT("the Python process was destroyed"));
	Handler.Reset(); //Threaded CLISystem handler: terminates the process if still running and joins its threads
}

void FPythonProcess::OpenStream(const TSharedRef<FPythonStream, ESPMode::ThreadSafe>& Stream)
{
	Channel->AddStream(Stream);
	if (Handler.IsValid() && (bStarting || bRunning))
	{
		Handler->SendInput(PythonWire::EncodeFrame(PythonWire::Control, Stream->MakeOpenHeader()));
	}
	//Otherwise Launch() opens it
}

void FPythonProcess::CloseStream(const FString& Owner, const FString& StreamName)
{
	Channel->RemoveStream(Owner + TEXT("/") + StreamName);
	if (Handler.IsValid() && (bStarting || bRunning))
	{
		TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
		Header->SetStringField(TEXT("cmd"), TEXT("closeStream"));
		Header->SetStringField(TEXT("owner"), Owner);
		Header->SetStringField(TEXT("stream"), StreamName);
		Handler->SendInput(PythonWire::EncodeFrame(PythonWire::Control, Header));
	}
}

void FPythonProcess::Start(TSharedPtr<FPythonEnvironment, ESPMode::ThreadSafe> InEnvironment)
{
	if (bStarting || bRunning)
	{
		return;
	}
	Environment = InEnvironment;
	bStarting = true;
	bStopping = false;
	bEverStarted = true;
	const uint32 Gen = ++Generation;
	TWeakPtr<FPythonProcess, ESPMode::ThreadSafe> Weak = AsShared();
	Environment->EnsureReady([Weak, Gen](bool bOk, const FString& Error)
	{
		TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> Self = Weak.Pin();
		if (!Self || Self->Generation != Gen || !Self->bStarting)
		{
			return;
		}
		if (bOk)
		{
			Self->Launch();
		}
		else
		{
			Self->HandleExit(-1, FString::Printf(TEXT("Python environment is not available: %s"), *Error));
		}
	});
}

void FPythonProcess::Launch()
{
	using namespace PythonProcessPrivate;
	const UPythonCLISettings* Settings = GetDefault<UPythonCLISettings>();

	// Runtime config: read by runtime.py before anything is imported
	TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
	Config->SetArrayField(TEXT("site_dirs"), { MakeShared<FJsonValueString>(Environment->SitePackagesDir()) });
	TArray<TSharedPtr<FJsonValue>> ScriptDirs;
	for (const FString& Dir : FPythonEnvironment::GetScriptDirectories())
	{
		ScriptDirs.Add(MakeShared<FJsonValueString>(Dir));
	}
	Config->SetArrayField(TEXT("script_dirs"), ScriptDirs);
	TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : FPythonEnvironment::GetEnvironmentVariables())
	{
		Env->SetStringField(Pair.Key, Pair.Value);
	}
	for (const TPair<FString, FString>& Pair : Settings->EnvironmentVariables) //The project's settings win
	{
		Env->SetStringField(Pair.Key, Pair.Value);
	}
	Config->SetObjectField(TEXT("env"), Env);
	TArray<TSharedPtr<FJsonValue>> Preload;
	if (bPreload)
	{
		for (const FString& Module : Settings->PreloadModules)
		{
			Preload.Add(MakeShared<FJsonValueString>(Module));
		}
	}
	Config->SetArrayField(TEXT("preload"), Preload);
	Config->SetStringField(TEXT("project_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Config->SetStringField(TEXT("content_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
	Config->SetStringField(TEXT("process"), Name);

	const FString ConfigPath = FPythonEnvironment::WorkDir() / FString::Printf(TEXT("runtime_%s.json"), *Name);
	FFileHelper::SaveStringToFile(PythonWire::ToJsonString(MakeShared<FJsonValueObject>(Config)), *ConfigPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	const FString Interpreter = FPythonEnvironment::InterpreterPath();
	FPythonEnvironment::EnsureExecutable(Interpreter);

	FProcessParams P;
	P.Url = Interpreter;
	P.Params = FString::Printf(TEXT("-I -u -X utf8 \"%s\" --config \"%s\""), *FPythonEnvironment::RuntimeScriptPath(), *ConfigPath);
	P.OptionalWorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	P.bProcessInBytes = true;
	P.bOutputToGameThread = false;
	P.bBlockingRead = true;
	P.bAsyncStdinWrites = true;
	P.bLaunchHidden = true;
	P.bLaunchReallyHidden = true;

	const uint32 Gen = Generation;
	TSharedPtr<FReaderState, ESPMode::ThreadSafe> State = MakeShared<FReaderState, ESPMode::ThreadSafe>();
	Reader = State;
	TWeakPtr<FPythonProcess, ESPMode::ThreadSafe> Weak = AsShared();

	Handler = MakeShared<FSubProcessHandler>();
	Handler->OnProcessBegin = [Weak, Gen](int32 InPid, bool bOk)
	{
		TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> Self = Weak.Pin();
		if (!Self || Self->Generation != Gen)
		{
			return;
		}
		if (!bOk)
		{
			Self->HandleExit(-1, FString::Printf(TEXT("could not launch %s"), *FPythonEnvironment::InterpreterPath()));
			return;
		}
		Self->Pid = InPid;
		Self->bStarting = false;
		Self->bRunning = true;
	};
	Handler->OnProcessEnd = [Weak, Gen](int32, int32 Code)
	{
		TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> Self = Weak.Pin();
		if (Self && Self->Generation == Gen)
		{
			Self->HandleExit(Code, Self->bStopping ? FString() : FString::Printf(TEXT("Python process '%s' exited (code %d)"), *Self->Name, Code));
		}
	};
	Handler->OnProcessOutput = [](int32, const FString&) {};
	Handler->OnProcessOutputBytes = [Weak, Gen, State, InChannel = Channel](int32, const TArray<uint8>& Bytes)
	{
		//Pipe thread: decode; stream ACKs and step replies are handled right here, the rest goes to the game thread
		State->Decoder.Feed(Bytes,
			[&Weak, Gen, &State, &InChannel](FCLIFrame& Raw)
			{
				FPythonFrame Frame;
				Frame.Type = (PythonWire::EFrameType)Raw.Type;
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Raw.HeaderAsString()), Frame.Header);
				if (!Frame.Header.IsValid())
				{
					Frame.Header = MakeShared<FJsonObject>();
				}
				if (Frame.Type == PythonWire::Status && Frame.Header->GetStringField(TEXT("status")) == TEXT("exiting"))
				{
					State->bExiting = true;
				}
				if (InChannel->HandleOnPipeThread(Gen, Raw.Type, Frame.Header, Raw.Binary))
				{
					return;
				}
				Frame.Binary = MoveTemp(Raw.Binary);
				AsyncTask(ENamedThreads::GameThread, [Weak, Gen, Frame = MoveTemp(Frame)]() mutable
				{
					TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> Self = Weak.Pin();
					if (Self && Self->Generation == Gen)
					{
						Self->HandleFrame(MoveTemp(Frame));
					}
				});
			},
			[&Weak](TConstArrayView<uint8> Stray)
			{
				//Text outside frames: early startup errors or a crash dump (faulthandler)
				const FString Text = PythonProcessPrivate::Utf8ToString(Stray.GetData(), Stray.Num());
				AsyncTask(ENamedThreads::GameThread, [Weak, Text]
				{
					if (TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> Self = Weak.Pin())
					{
						Self->LogText(Self->StrayText, Text, true);
					}
				});
			});
	};

	UE_LOG(LogPythonCLI, Log, TEXT("Starting Python process '%s'"), *Name);
	Handler->StartProcess(P);
	//Async writes: the writer holds these until the process is up. Streams first, so Python has mapped
	//them before any frame of theirs arrives.
	for (const TSharedRef<FPythonStream, ESPMode::ThreadSafe>& Stream : Channel->GetStreams())
	{
		Handler->SendInput(PythonWire::EncodeFrame(PythonWire::Control, Stream->MakeOpenHeader()));
	}
	for (TArray<uint8>& Frame : Pending)
	{
		Handler->SendInput(Frame);
	}
	Pending.Reset();
	Channel->Attach(Handler, Gen);
}

void FPythonProcess::Send(TArray<uint8>&& Frame)
{
	if (Handler.IsValid() && (bStarting || bRunning))
	{
		Handler->SendInput(Frame);
		return;
	}
	Pending.Add(MoveTemp(Frame));
	if (!bStarting && !bRunning && bEverStarted && !bStopping && Environment.IsValid())
	{
		Start(Environment); //It exited (crash or stop): bring it back for whoever is still using it
	}
}

void FPythonProcess::AddOwner(const FString& Owner, TFunction<void(const FPythonFrame&)> OnFrame, TFunction<void(int32, const FString&)> OnExit)
{
	Owners.Add(Owner, FOwner{ MoveTemp(OnFrame), MoveTemp(OnExit) });
}

void FPythonProcess::RemoveOwner(const FString& Owner)
{
	if (Owners.Remove(Owner) > 0 && (bStarting || bRunning))
	{
		TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
		Header->SetStringField(TEXT("cmd"), TEXT("stopOwner"));
		Header->SetStringField(TEXT("owner"), Owner);
		Send(PythonWire::EncodeFrame(PythonWire::Control, Header));
	}
}

void FPythonProcess::HandleFrame(FPythonFrame&& Frame)
{
	using namespace PythonProcessPrivate;
	const TSharedPtr<FJsonObject>& H = Frame.Header;
	FString Owner;
	H->TryGetStringField(TEXT("owner"), Owner);

	switch (Frame.Type)
	{
	case PythonWire::RawLog:
		LogText(NativeText, Utf8ToString(Frame.Binary.GetData(), Frame.Binary.Num()), false);
		return;
	case PythonWire::Status:
	{
		const FString Status = H->GetStringField(TEXT("status"));
		if (Status == TEXT("ready"))
		{
			bReady = true;
			bStarting = false;
			bRunning = true;
			FString Numpy;
			H->TryGetStringField(TEXT("numpy"), Numpy);
			double Ms = 0;
			H->TryGetNumberField(TEXT("startup_ms"), Ms);
			UE_LOG(LogPythonCLI, Display, TEXT("Python process '%s' ready: Python %s, numpy %s, pid %d, %.0f ms"),
				*Name, *H->GetStringField(TEXT("python")), Numpy.IsEmpty() ? TEXT("not installed") : *Numpy, (int32)H->GetNumberField(TEXT("pid")), Ms);
			FString NumpyError;
			if (H->TryGetStringField(TEXT("numpy_error"), NumpyError) && !NumpyError.IsEmpty())
			{
				UE_LOG(LogPythonCLI, Warning, TEXT("numpy is installed but failed to import: %s"), *NumpyError);
			}
			OnReady.Broadcast();
			return;
		}
		if (Status == TEXT("preloaded"))
		{
			UE_LOG(LogPythonCLI, Display, TEXT("Python process '%s' preloaded modules in %.2f s: %s"), *Name,
				H->GetNumberField(TEXT("seconds")), *PythonWire::ToJsonString(H->TryGetField(TEXT("modules"))));
			const TSharedPtr<FJsonObject>* Failed = nullptr;
			if (H->TryGetObjectField(TEXT("failed"), Failed) && (*Failed)->Values.Num() > 0)
			{
				UE_LOG(LogPythonCLI, Warning, TEXT("Preloading failed: %s"), *PythonWire::ToJsonString(MakeShared<FJsonValueObject>(*Failed)));
			}
			return;
		}
		break;
	}
	case PythonWire::Log:
		if (Owner.IsEmpty() || !Owners.Contains(Owner))
		{
			const FString Level = H->GetStringField(TEXT("level"));
			LogLines(Level == TEXT("error") ? ELogVerbosity::Error : Level == TEXT("warning") ? ELogVerbosity::Warning : ELogVerbosity::Log,
				TEXT("[python] "), H->GetStringField(TEXT("msg")));
			return;
		}
		break;
	default:
		break;
	}

	if (FOwner* Target = Owners.Find(Owner))
	{
		Target->OnFrame(Frame);
	}
}

void FPythonProcess::LogText(FString& Buffer, const FString& Text, bool bWarning, bool bFlush)
{
	Buffer += Text;
	int32 Last = INDEX_NONE;
	const TCHAR* Prefix = bWarning ? TEXT("[python] ") : TEXT("[python native] ");
	const ELogVerbosity::Type Verbosity = bWarning ? ELogVerbosity::Warning : ELogVerbosity::Log;
	if (bFlush)
	{
		PythonProcessPrivate::LogLines(Verbosity, Prefix, Buffer);
		Buffer.Reset();
	}
	else if (Buffer.FindLastChar(TEXT('\n'), Last))
	{
		PythonProcessPrivate::LogLines(Verbosity, Prefix, Buffer.Left(Last));
		Buffer.RightChopInline(Last + 1);
	}
}

void FPythonProcess::HandleExit(int32 Code, const FString& Message)
{
	LogText(NativeText, FString(), false, true);
	LogText(StrayText, FString(), true, true);
	const bool bWasStopping = bStopping;
	bStarting = false;
	bRunning = false;
	bReady = false;
	Pid = 0;
	++Generation; //Anything still in flight from this launch is stale now
	Channel->Detach(Message.IsEmpty() ? FString::Printf(TEXT("Python process '%s' exited"), *Name) : Message);
	Release(0.5f);
	if (!Message.IsEmpty())
	{
		UE_LOG(LogPythonCLI, Warning, TEXT("%s"), *Message);
	}
	if (bWasStopping)
	{
		return;
	}
	TArray<FOwner> Notify;
	Owners.GenerateValueArray(Notify);
	for (FOwner& O : Notify)
	{
		O.OnExit(Code, Message);
	}
}

void FPythonProcess::Release(float DelaySeconds)
{
	if (Handler.IsValid())
	{
		//Not from inside the handler's own callback: hand it to CLISystem to destroy later (it joins its threads)
		FSubProcessHandler::Retire(MoveTemp(Handler), DelaySeconds);
	}
	Reader.Reset();
}

void FPythonProcess::Stop(float GraceSeconds, bool bWait)
{
	Pending.Reset();
	if (!Handler.IsValid() || (!bStarting && !bRunning))
	{
		bStarting = false;
		Release(0.f);
		return;
	}
	bStopping = true;
	Channel->Detach(FString::Printf(TEXT("Python process '%s' was stopped"), *Name));
	TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
	Header->SetStringField(TEXT("cmd"), TEXT("exit"));
	Header->SetNumberField(TEXT("grace"), FMath::RoundToInt(GraceSeconds * 1000.f));
	Handler->SendInput(PythonWire::EncodeFrame(PythonWire::Control, Header));

	if (bWait)
	{
		const double Deadline = FPlatformTime::Seconds() + GraceSeconds + 0.5;
		while (Reader.IsValid() && !Reader->bExiting && FPlatformTime::Seconds() < Deadline)
		{
			FPlatformProcess::Sleep(0.005f);
		}
		++Generation;
		Handler.Reset(); //Terminates it if it hasn't exited yet and joins the pipe threads
		Reader.Reset();
	}
	else
	{
		++Generation;
		Release(GraceSeconds + 0.5f);
	}
	bStarting = false;
	bRunning = false;
	bReady = false;
	Pid = 0;

	//Owners' scripts are gone with it
	TArray<FOwner> Notify;
	Owners.GenerateValueArray(Notify);
	const FString Message = FString::Printf(TEXT("Python process '%s' was stopped"), *Name);
	for (FOwner& O : Notify)
	{
		O.OnExit(0, Message);
	}
}
