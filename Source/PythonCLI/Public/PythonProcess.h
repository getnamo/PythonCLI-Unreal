#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "PythonTypes.h"

class FPythonEnvironment;
class FSubProcessHandler;

/** One frame from Python (game thread). */
struct FPythonFrame
{
	PythonWire::EFrameType Type = PythonWire::Log;
	TSharedPtr<FJsonObject> Header;
	TArray<uint8> Binary;
};

/**
 * A Python process running Content/Python/runtime.py with the bundled interpreter. It hosts scripts for any
 * number of owners (components): frames from Python are routed to their owner on the game thread. The
 * shared process (UPythonSubsystem) hosts every component that uses it; a dedicated process hosts one.
 * Frames sent before the process is up are queued. Game thread only.
 */
class PYTHONCLI_API FPythonProcess : public TSharedFromThis<FPythonProcess, ESPMode::ThreadSafe>
{
public:
	explicit FPythonProcess(const FString& InName, bool bInPreload = false);
	~FPythonProcess();

	/** Launch once the environment is ready (sets it up first if needed). No-op while starting or running. */
	void Start(TSharedPtr<FPythonEnvironment, ESPMode::ThreadSafe> InEnvironment);

	/** Ask scripts to run on_stop and the process to exit. With bWait the game thread waits up to
	 GraceSeconds for it; otherwise the process is released in the background after GraceSeconds. */
	void Stop(float GraceSeconds, bool bWait = false);

	/** Send a frame (queued until the process is up). Restarts a process that exited if it was started before. */
	void Send(TArray<uint8>&& Frame);

	/** Route frames addressed to Owner here, and tell it when the process exits (ExitCode, or -1 if it
	 couldn't start; the message explains). */
	void AddOwner(const FString& Owner, TFunction<void(const FPythonFrame&)> OnFrame, TFunction<void(int32, const FString&)> OnExit);
	/** Stop the owner's scripts and stop routing to it. */
	void RemoveOwner(const FString& Owner);

	bool IsStarting() const { return bStarting; }
	bool IsRunning() const { return bRunning; }
	bool IsReady() const { return bReady; }
	int32 GetPid() const { return Pid; }
	const FString& GetName() const { return Name; }

	FSimpleMulticastDelegate OnReady;

private:
	void Launch();
	void HandleFrame(FPythonFrame&& Frame);
	void HandleExit(int32 Code, const FString& Message);
	/** Native output and stray text arrive in arbitrary pieces: log complete lines only. */
	void LogText(FString& Buffer, const FString& Text, bool bWarning, bool bFlush = false);
	void Release(float DelaySeconds);

	FString Name;
	bool bPreload = false;
	TSharedPtr<FPythonEnvironment, ESPMode::ThreadSafe> Environment;
	TSharedPtr<FSubProcessHandler> Handler;
	TArray<TArray<uint8>> Pending;

	struct FOwner
	{
		TFunction<void(const FPythonFrame&)> OnFrame;
		TFunction<void(int32, const FString&)> OnExit;
	};
	TMap<FString, FOwner> Owners;

	/** State the pipe thread uses; one per launch. */
	struct FReaderState;
	TSharedPtr<FReaderState, ESPMode::ThreadSafe> Reader;

	FString StrayText;
	FString NativeText;

	bool bStarting = false;
	bool bRunning = false;
	bool bReady = false;
	bool bStopping = false;
	bool bEverStarted = false;
	int32 Pid = 0;
	uint32 Generation = 0;
};
