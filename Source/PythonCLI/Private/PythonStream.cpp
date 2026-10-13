#include "PythonStream.h"
#include "PythonCLI.h"
#include "PythonProcess.h"
#include "Dom/JsonObject.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"

namespace PythonStreamPrivate
{
	constexpr int64 SharedMemoryThreshold = 32 * 1024;
	constexpr int32 MaxSlots = 64;

	static const TCHAR* PolicyName(EPythonStreamPolicy Policy)
	{
		switch (Policy)
		{
		case EPythonStreamPolicy::Queue: return TEXT("queue");
		case EPythonStreamPolicy::Lockstep: return TEXT("lockstep");
		default: return TEXT("latest");
		}
	}
}

TSharedPtr<FPythonStream, ESPMode::ThreadSafe> FPythonStream::Open(const TSharedRef<FPythonProcess, ESPMode::ThreadSafe>& Process, const FPythonStreamConfig& Config, FString& OutError)
{
	using namespace PythonStreamPrivate;
	if (Config.Owner.IsEmpty() || Config.Name.IsEmpty())
	{
		OutError = TEXT("a stream needs an owner and a name");
		return nullptr;
	}
	if (Config.SlotBytes <= 0)
	{
		OutError = FString::Printf(TEXT("stream '%s': frame size must be > 0"), *Config.Name);
		return nullptr;
	}

	TSharedRef<FPythonStream, ESPMode::ThreadSafe> S = MakeShared<FPythonStream, ESPMode::ThreadSafe>();
	S->Config = Config;
	S->Config.Slots = FMath::Clamp(Config.Slots, 1, MaxSlots);
	const bool bShared = Config.Transport == EPythonStreamTransport::SharedMemory
		|| (Config.Transport == EPythonStreamTransport::Auto && Config.SlotBytes >= SharedMemoryThreshold);
	if (bShared)
	{
		S->SlotBytes = Align(Config.SlotBytes, 64); //Slots start cache-line (and SIMD) aligned in Python
		//A {GUID} name: on Windows any other name gets a Global\ prefix, which needs admin rights
		S->ShmName = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensInBraces);
		S->Region = FPlatformMemory::MapNamedSharedMemoryRegion(S->ShmName, true,
			FPlatformMemory::ESharedMemoryAccess::Read | FPlatformMemory::ESharedMemoryAccess::Write, S->SlotBytes * S->Config.Slots);
		if (!S->Region)
		{
			OutError = FString::Printf(TEXT("stream '%s': could not create %lld bytes of shared memory"), *Config.Name, S->SlotBytes * S->Config.Slots);
			return nullptr;
		}
	}
	else
	{
		S->SlotBytes = Config.SlotBytes;
		S->InlineSlots.SetNum(S->Config.Slots);
		for (TArray<uint8>& Slot : S->InlineSlots)
		{
			Slot.SetNumUninitialized(S->SlotBytes);
		}
	}
	S->Slots.Init(ESlot::Free, S->Config.Slots);
	S->SentAt.Init(0.0, S->Config.Slots);
	S->SlotFreed = FPlatformProcess::GetSynchEventFromPool(false);
	S->Process = Process;
	S->Channel = Process->GetChannel();
	Process->OpenStream(S);
	return S;
}

FPythonStream::~FPythonStream()
{
	if (!bClosed)
	{
		//Not from the game thread necessarily: just stop routing; Python drops it with its owner
		bClosed = true;
		if (Channel.IsValid())
		{
			Channel->RemoveStream(GetKey());
		}
	}
	if (Region)
	{
		FPlatformMemory::UnmapNamedSharedMemoryRegion(Region);
		Region = nullptr;
	}
	if (SlotFreed)
	{
		FPlatformProcess::ReturnSynchEventToPool(SlotFreed);
		SlotFreed = nullptr;
	}
}

void FPythonStream::Close()
{
	if (bClosed.exchange(true))
	{
		return;
	}
	SlotFreed->Trigger(); //Wakes a producer waiting for a slot: it sees the stream closed

	//A producer on another thread may be between AcquireSlot and Commit: let it finish with the memory
	const double Deadline = FPlatformTime::Seconds() + 2.0;
	while (true)
	{
		{
			FScopeLock L(&Lock);
			if (!Slots.Contains(ESlot::Writing))
			{
				break;
			}
		}
		if (FPlatformTime::Seconds() > Deadline)
		{
			UE_LOG(LogPythonCLI, Warning, TEXT("Python stream '%s': a producer is still writing a slot; closing anyway"), *Config.Name);
			break;
		}
		FPlatformProcess::SleepNoStats(0.001f);
	}

	if (TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> P = Process.Pin())
	{
		P->CloseStream(Config.Owner, Config.Name);
	}
	else if (Channel.IsValid())
	{
		Channel->RemoveStream(GetKey());
	}
	//Python may still hold views of the region: the OS keeps it alive until they are gone
	if (Region)
	{
		FPlatformMemory::UnmapNamedSharedMemoryRegion(Region);
		Region = nullptr;
	}
}

int32 FPythonStream::AcquireSlot(double WaitSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + FMath::Max(WaitSeconds, 0.0);
	while (true)
	{
		{
			FScopeLock L(&Lock);
			if (bClosed)
			{
				return INDEX_NONE;
			}
			const int32 Free = Slots.IndexOfByKey(ESlot::Free);
			if (Free != INDEX_NONE)
			{
				Slots[Free] = ESlot::Writing;
				return Free;
			}
			if (FPlatformTime::Seconds() >= Deadline)
			{
				++Dropped;
				return INDEX_NONE;
			}
		}
		const double Left = Deadline - FPlatformTime::Seconds();
		if (Left > 0.0)
		{
			SlotFreed->Wait(FTimespan::FromSeconds(Left));
		}
	}
}

uint8* FPythonStream::GetSlotData(int32 Slot)
{
	if (!Slots.IsValidIndex(Slot))
	{
		return nullptr;
	}
	if (Region)
	{
		return static_cast<uint8*>(Region->GetAddress()) + Slot * SlotBytes;
	}
	return InlineSlots.IsValidIndex(Slot) ? InlineSlots[Slot].GetData() : nullptr;
}

void FPythonStream::Cancel(int32 Slot)
{
	{
		FScopeLock L(&Lock);
		if (Slots.IsValidIndex(Slot) && Slots[Slot] == ESlot::Writing)
		{
			Slots[Slot] = ESlot::Free;
		}
	}
	SlotFreed->Trigger();
}

bool FPythonStream::Commit(int32 Slot, const FPythonStreamFrame& Frame)
{
	return CommitInternal(Slot, Frame, -1);
}

bool FPythonStream::CommitInternal(int32 Slot, const FPythonStreamFrame& Frame, int64 StepId)
{
	if (!Slots.IsValidIndex(Slot))
	{
		return false;
	}
	if (bClosed || Frame.NumBytes < 0 || Frame.NumBytes > SlotBytes)
	{
		if (!bClosed)
		{
			UE_LOG(LogPythonCLI, Error, TEXT("Python stream '%s': frame of %lld bytes doesn't fit its %lld-byte slot"), *Config.Name, Frame.NumBytes, SlotBytes);
		}
		Cancel(Slot);
		return false;
	}

	TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
	H->SetStringField(TEXT("owner"), Config.Owner);
	H->SetStringField(TEXT("stream"), Config.Name);
	H->SetNumberField(TEXT("seq"), (double)NextSeq++);
	H->SetNumberField(TEXT("slot"), Slot);
	H->SetNumberField(TEXT("t"), Frame.Time > 0.0 ? Frame.Time : FPlatformTime::Seconds());
	H->SetNumberField(TEXT("frame"), (double)(Frame.FrameNumber ? Frame.FrameNumber : GFrameCounter));
	H->SetStringField(TEXT("dtype"), FPyTensor::DTypeName(Frame.DType));
	TArray<TSharedPtr<FJsonValue>> Shape;
	for (const int64 Dim : Frame.Shape)
	{
		Shape.Add(MakeShared<FJsonValueNumber>((double)Dim));
	}
	H->SetArrayField(TEXT("shape"), Shape);
	H->SetNumberField(TEXT("nbytes"), (double)Frame.NumBytes);
	if (!Frame.Layout.IsEmpty())
	{
		H->SetStringField(TEXT("layout"), Frame.Layout);
	}
	if (Frame.Meta.IsValid())
	{
		H->SetObjectField(TEXT("meta"), Frame.Meta);
	}
	if (StepId >= 0)
	{
		H->SetNumberField(TEXT("sid"), (double)StepId);
	}
	TConstArrayView<uint8> Inline;
	if (!Region)
	{
		Inline = TConstArrayView<uint8>(InlineSlots[Slot].GetData(), (int32)Frame.NumBytes);
	}
	const TArray<uint8> Bytes = PythonWire::EncodeFrame(PythonWire::Stream, H, Inline);

	{
		FScopeLock L(&Lock); //In flight before it's sent: the ACK can come back before Send returns
		Slots[Slot] = ESlot::InFlight;
		SentAt[Slot] = FPlatformTime::Seconds();
	}
	if (!Channel->Send(Bytes))
	{
		{
			FScopeLock L(&Lock);
			if (Slots[Slot] == ESlot::InFlight)
			{
				Slots[Slot] = ESlot::Free;
			}
			++Dropped;
		}
		SlotFreed->Trigger();
		return false;
	}
	FScopeLock L(&Lock);
	++Sent;
	return true;
}

bool FPythonStream::Push(const void* Data, const FPythonStreamFrame& Frame, double WaitSeconds)
{
	if (WaitSeconds < 0.0)
	{
		WaitSeconds = Config.Policy == EPythonStreamPolicy::Latest ? 0.0 : MaxWaitSeconds;
	}
	if (Frame.NumBytes > SlotBytes)
	{
		UE_LOG(LogPythonCLI, Error, TEXT("Python stream '%s': frame of %lld bytes doesn't fit its %lld-byte slot"), *Config.Name, Frame.NumBytes, SlotBytes);
		return false;
	}
	const int32 Slot = AcquireSlot(WaitSeconds);
	if (Slot == INDEX_NONE)
	{
		return false;
	}
	if (Frame.NumBytes > 0)
	{
		FMemory::Memcpy(GetSlotData(Slot), Data, Frame.NumBytes);
	}
	return Commit(Slot, Frame);
}

bool FPythonStream::Step(const void* Data, const FPythonStreamFrame& Frame, double TimeoutSeconds, FPythonCallResult& OutResult)
{
	OutResult = FPythonCallResult();
	const double Start = FPlatformTime::Seconds();
	if (Frame.NumBytes > SlotBytes)
	{
		OutResult.Error = FString::Printf(TEXT("frame of %lld bytes doesn't fit the %lld-byte slot"), Frame.NumBytes, SlotBytes);
		return false;
	}
	if (!Channel->IsRunning())
	{
		OutResult.Error = TEXT("the Python process is not running (start the script first)");
		return false;
	}
	const int32 Slot = AcquireSlot(TimeoutSeconds);
	if (Slot == INDEX_NONE)
	{
		OutResult.Error = bClosed ? TEXT("the stream is closed") : TEXT("no free slot: Python is still busy with earlier frames");
		return false;
	}
	if (Frame.NumBytes > 0)
	{
		FMemory::Memcpy(GetSlotData(Slot), Data, Frame.NumBytes);
	}
	const int64 Id = Channel->BeginWait();
	if (!CommitInternal(Slot, Frame, Id))
	{
		Channel->CancelWait(Id);
		OutResult.Error = TEXT("the Python process is not running");
		return false;
	}
	return Channel->Wait(Id, TimeoutSeconds - (FPlatformTime::Seconds() - Start), OutResult);
}

void FPythonStream::HandleAck(int32 Slot)
{
	{
		FScopeLock L(&Lock);
		if (!Slots.IsValidIndex(Slot) || Slots[Slot] != ESlot::InFlight)
		{
			return;
		}
		Slots[Slot] = ESlot::Free;
		++Acked;
		const double Ms = (FPlatformTime::Seconds() - SentAt[Slot]) * 1000.0;
		AckLatencyMs = Acked == 1 ? Ms : AckLatencyMs * 0.9 + Ms * 0.1;
	}
	SlotFreed->Trigger();
}

void FPythonStream::ResetSlots()
{
	{
		FScopeLock L(&Lock);
		for (ESlot& S : Slots)
		{
			if (S == ESlot::InFlight)
			{
				S = ESlot::Free;
			}
		}
	}
	SlotFreed->Trigger();
}

FPythonStreamStats FPythonStream::GetStats() const
{
	FScopeLock L(&Lock);
	FPythonStreamStats S;
	S.Sent = Sent;
	S.Dropped = Dropped;
	S.Acked = Acked;
	S.InFlight = Slots.FilterByPredicate([](ESlot X) { return X == ESlot::InFlight; }).Num();
	S.AckLatencyMs = (float)AckLatencyMs;
	return S;
}

TSharedRef<FJsonObject> FPythonStream::MakeOpenHeader() const
{
	TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
	H->SetStringField(TEXT("cmd"), TEXT("openStream"));
	H->SetStringField(TEXT("owner"), Config.Owner);
	H->SetStringField(TEXT("script"), Config.Script);
	H->SetStringField(TEXT("stream"), Config.Name);
	H->SetNumberField(TEXT("slots"), Config.Slots);
	H->SetNumberField(TEXT("slot_bytes"), (double)SlotBytes);
	H->SetStringField(TEXT("policy"), PythonStreamPrivate::PolicyName(Config.Policy));
	if (Region)
	{
		H->SetStringField(TEXT("shm"), ShmName);
	}
	H->SetObjectField(TEXT("info"), Config.Info.IsValid() ? Config.Info : MakeShared<FJsonObject>());
	return H;
}
