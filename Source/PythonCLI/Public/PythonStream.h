#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformMemory.h"
#include "PythonTypes.h"
#include <atomic>
#include "PythonStream.generated.h"

class FPythonChannel;
class FPythonProcess;
class FJsonObject;

/** How a stream behaves when Python is slower than Unreal. */
UENUM(BlueprintType)
enum class EPythonStreamPolicy : uint8
{
	/** Live data: Unreal drops new frames while every slot is in use, and Python skips frames a newer one has
	 superseded. Handlers always see the freshest data; nothing ever waits. */
	Latest,
	/** Every frame is delivered, in order. A producer waits (up to MaxWaitSeconds) for a free slot. */
	Queue,
	/** Step(): Unreal sends a frame and waits for the Python handler's return value (deterministic ML tests). */
	Lockstep,
};

/** Where frame data travels. */
UENUM(BlueprintType)
enum class EPythonStreamTransport : uint8
{
	/** Shared memory for frames of 32 KB or more, inline below that. */
	Auto,
	/** A named shared-memory region with one slot per in-flight frame (zero-copy in Python). */
	SharedMemory,
	/** In the frame itself (small data). */
	Inline,
};

USTRUCT(BlueprintType)
struct PYTHONCLI_API FPythonStreamStats
{
	GENERATED_BODY()

	/** Frames sent to Python. */
	UPROPERTY(BlueprintReadOnly, Category = "Python Stream")
	int64 Sent = 0;

	/** Frames Unreal dropped: no free slot (Latest, or Queue after MaxWaitSeconds), or no Python process. */
	UPROPERTY(BlueprintReadOnly, Category = "Python Stream")
	int64 Dropped = 0;

	/** Frames Python handed back. */
	UPROPERTY(BlueprintReadOnly, Category = "Python Stream")
	int64 Acked = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Python Stream")
	int32 InFlight = 0;

	/** Average time from send to ACK (ms; recent frames weigh more). */
	UPROPERTY(BlueprintReadOnly, Category = "Python Stream")
	float AckLatencyMs = 0.f;
};

/** How to open a stream (C++). */
struct FPythonStreamConfig
{
	FString Owner;
	FString Script;
	FString Name;
	EPythonStreamPolicy Policy = EPythonStreamPolicy::Latest;
	EPythonStreamTransport Transport = EPythonStreamTransport::Auto;
	int32 Slots = 3;
	/** Largest frame. */
	int64 SlotBytes = 0;
	/** Fixed description Python sees as frame.info (feature names, sample rate, ...). */
	TSharedPtr<FJsonObject> Info;
};

/** One frame's description. */
struct FPythonStreamFrame
{
	EPyDType DType = EPyDType::UInt8;
	TArray<int64> Shape;
	int64 NumBytes = 0;
	/** Channel layout of images ("BGRA", "RGBA", "R"); empty otherwise. */
	FString Layout;
	/** Per-frame values (frame.meta in Python). */
	TSharedPtr<FJsonObject> Meta;
	/** When the data was captured (FPlatformTime::Seconds()) and GFrameCounter then; 0 = now. */
	double Time = 0.0;
	uint64 FrameNumber = 0;
};

/**
 * A stream of frames to a Python script's @ue.on_stream(name) handlers (or ue.stream(name) iterator).
 *
 * The stream has a fixed number of slots, each one a credit: a producer acquires a free slot, writes it,
 * and commits it (a STREAM frame); the slot comes back when Python ACKs it, after its handler returns.
 * Producers may run on any thread (render thread, audio thread), one at a time per stream. ACKs and
 * lockstep replies are handled on the pipe thread, so a game thread blocked in Step() still gets them.
 */
class PYTHONCLI_API FPythonStream : public TSharedFromThis<FPythonStream, ESPMode::ThreadSafe>
{
public:
	/** Game thread. Creates the shared memory (if used) and tells Python; Python maps it before any frame. */
	static TSharedPtr<FPythonStream, ESPMode::ThreadSafe> Open(const TSharedRef<FPythonProcess, ESPMode::ThreadSafe>& Process, const FPythonStreamConfig& Config, FString& OutError);
	~FPythonStream();

	/** Game thread. Waits for a producer still writing a slot, tells Python, and releases the memory. */
	void Close();
	bool IsOpen() const { return !bClosed; }

	/** A free slot, or INDEX_NONE (counted as dropped) if none frees up within WaitSeconds. Any thread. */
	int32 AcquireSlot(double WaitSeconds);
	/** The slot's memory (SlotBytes long) between AcquireSlot and Commit/Cancel. */
	uint8* GetSlotData(int32 Slot);
	/** Send the slot to Python. False if the process isn't running (the slot is freed; counted as dropped). */
	bool Commit(int32 Slot, const FPythonStreamFrame& Frame);
	void Cancel(int32 Slot);

	/** AcquireSlot + copy + Commit. WaitSeconds < 0: by policy (Latest 0, otherwise the configured wait). */
	bool Push(const void* Data, const FPythonStreamFrame& Frame, double WaitSeconds = -1.0);

	/** Send a frame and wait for the Python handler's return value (Lockstep). Any thread; usually the game
	 thread, which stays blocked until the reply arrives or TimeoutSeconds pass. */
	bool Step(const void* Data, const FPythonStreamFrame& Frame, double TimeoutSeconds, FPythonCallResult& OutResult);

	FPythonStreamStats GetStats() const;
	const FPythonStreamConfig& GetConfig() const { return Config; }
	int64 GetSlotBytes() const { return SlotBytes; }
	bool UsesSharedMemory() const { return Region != nullptr; }
	/** "owner/name": how ACKs find the stream. */
	FString GetKey() const { return Config.Owner + TEXT("/") + Config.Name; }

	/** How long Queue producers wait for a slot (Push with WaitSeconds < 0). */
	double MaxWaitSeconds = 1.0;

	/** The openStream control header (sent on open and again whenever the process (re)launches). */
	TSharedRef<FJsonObject> MakeOpenHeader() const;
	/** Pipe thread: Python is done with the slot. */
	void HandleAck(int32 Slot);
	/** The process ended: every slot is Unreal's again. */
	void ResetSlots();

private:
	enum class ESlot : uint8 { Free, Writing, InFlight };

	bool CommitInternal(int32 Slot, const FPythonStreamFrame& Frame, int64 StepId);

	FPythonStreamConfig Config;
	int64 SlotBytes = 0;
	FString ShmName;
	FPlatformMemory::FSharedMemoryRegion* Region = nullptr;
	TArray<TArray<uint8>> InlineSlots;
	TWeakPtr<FPythonProcess, ESPMode::ThreadSafe> Process;
	TSharedPtr<FPythonChannel, ESPMode::ThreadSafe> Channel;

	mutable FCriticalSection Lock;
	TArray<ESlot> Slots;
	TArray<double> SentAt;
	FEvent* SlotFreed = nullptr;
	std::atomic<bool> bClosed{ false };
	std::atomic<int64> NextSeq{ 0 };
	int64 Sent = 0;
	int64 Dropped = 0;
	int64 Acked = 0;
	double AckLatencyMs = 0.0;
};
