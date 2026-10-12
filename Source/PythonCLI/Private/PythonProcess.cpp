#include "PythonProcess.h"
#include "PythonEnvironment.h"
#include "PythonCLI.h"
#include "PythonCLISettings.h"
#include "Async/Async.h"
#include "CLIFrameCodec.h"
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

FPythonProcess::FPythonProcess(const FString& InName, bool bInPreload)
	: Name(InName)
	, bPreload(bInPreload)
{
}

FPythonProcess::~FPythonProcess()
{
	++Generation;
	Handler.Reset(); //Threaded CLISystem handler: terminates the process if still running and joins its threads
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
	TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Settings->EnvironmentVariables)
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

	const FString ConfigPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("PythonCLI") / FString::Printf(TEXT("runtime_%s.json"), *Name));
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
	Handler->OnProcessOutputBytes = [Weak, Gen, State](int32, const TArray<uint8>& Bytes)
	{
		//Pipe thread: decode, then hand each frame to the game thread
		State->Decoder.Feed(Bytes,
			[&Weak, Gen, &State](FCLIFrame& Raw)
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
	for (TArray<uint8>& Frame : Pending)
	{
		Handler->SendInput(Frame); //Async writes: the writer holds them until the process is up
	}
	Pending.Reset();
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
