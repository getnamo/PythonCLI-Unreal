#include "PythonAudioStreamComponent.h"
#include "PythonCLI.h"
#include "AudioCaptureCore.h"
#include "AudioDevice.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "ISubmixBufferListener.h"
#include "Sound/SoundSubmix.h"

namespace PythonAudioPrivate
{
	constexpr int32 MaxChannels = 8;
	constexpr int32 MinSampleRateHint = 48000;
}

/** Collects audio from any thread and pushes ChunkSeconds pieces; never waits for a slot. */
class UPythonAudioStreamComponent::FChunker : public TSharedFromThis<FChunker, ESPMode::ThreadSafe>
{
public:
	FChunker(TSharedPtr<FPythonStream, ESPMode::ThreadSafe> InStream, float InChunkSeconds, bool bInMono)
		: Stream(MoveTemp(InStream)), ChunkSeconds(InChunkSeconds), bMono(bInMono)
	{
	}

	void Add(const float* Data, int32 Frames, int32 Channels, int32 Rate)
	{
		using namespace PythonAudioPrivate;
		if (!Data || Frames <= 0 || Channels <= 0 || Rate <= 0)
		{
			return;
		}
		FScopeLock L(&Lock);
		if (!Stream.IsValid())
		{
			return;
		}
		const int32 OutChannels = bMono ? 1 : FMath::Min(Channels, MaxChannels);
		if (OutChannels != Channels_ || Rate != Rate_)
		{
			Pending.Reset(); //Format changed: start over
			Channels_ = OutChannels;
			Rate_ = Rate;
		}
		const int32 Start = Pending.Num();
		Pending.AddUninitialized(Frames * OutChannels);
		float* Out = Pending.GetData() + Start;
		for (int32 F = 0; F < Frames; ++F)
		{
			const float* In = Data + (int64)F * Channels;
			if (bMono)
			{
				float Sum = 0.f;
				for (int32 C = 0; C < Channels; ++C)
				{
					Sum += In[C];
				}
				*Out++ = Sum / Channels;
			}
			else
			{
				for (int32 C = 0; C < OutChannels; ++C)
				{
					*Out++ = In[C];
				}
			}
		}

		const int64 SlotFrames = Stream->GetSlotBytes() / (OutChannels * (int64)sizeof(float));
		const int32 ChunkFrames = (int32)FMath::Clamp<int64>(FMath::RoundToInt(ChunkSeconds * Rate), 1, FMath::Max<int64>(SlotFrames, 1));
		const int32 ChunkSamples = ChunkFrames * OutChannels;
		int32 Consumed = 0;
		while (Pending.Num() - Consumed >= ChunkSamples)
		{
			FPythonStreamFrame Frame;
			Frame.DType = EPyDType::Float32;
			Frame.Shape = { ChunkFrames, OutChannels };
			Frame.NumBytes = (int64)ChunkSamples * sizeof(float);
			Frame.Meta = MakeShared<FJsonObject>();
			Frame.Meta->SetNumberField(TEXT("sample_rate"), Rate);
			Stream->Push(Pending.GetData() + Consumed, Frame, 0.0);
			Consumed += ChunkSamples;
		}
		if (Consumed > 0)
		{
			Pending.RemoveAt(0, Consumed, EAllowShrinking::No);
		}
	}

	void Detach()
	{
		FScopeLock L(&Lock);
		Stream.Reset();
	}

private:
	FCriticalSection Lock;
	TSharedPtr<FPythonStream, ESPMode::ThreadSafe> Stream;
	float ChunkSeconds = 0.02f;
	bool bMono = false;
	TArray<float> Pending;
	int32 Channels_ = 0;
	int32 Rate_ = 0;
};

class UPythonAudioStreamComponent::FSubmixListener : public ISubmixBufferListener
{
public:
	explicit FSubmixListener(TSharedPtr<FChunker, ESPMode::ThreadSafe> InChunker) : Chunker(MoveTemp(InChunker)) {}

	virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override
	{
		if (NumChannels > 0)
		{
			Chunker->Add(AudioData, NumSamples / NumChannels, NumChannels, SampleRate);
		}
	}

	/** Keep the submix rendering while we listen, even when nothing plays (silence is data too). */
	virtual bool IsRenderingAudio() const override
	{
		return true;
	}

	virtual const FString& GetListenerName() const override
	{
		static const FString ListenerName = TEXT("PythonAudioStream");
		return ListenerName;
	}

private:
	TSharedPtr<FChunker, ESPMode::ThreadSafe> Chunker;
};

UPythonAudioStreamComponent::UPythonAudioStreamComponent()
{
	StreamName = TEXT("audio");
	Policy = EPythonStreamPolicy::Queue; //Python sees every chunk; the producer still never waits
	Slots = 16;
}

UPythonAudioStreamComponent::~UPythonAudioStreamComponent() = default;

int32 UPythonAudioStreamComponent::SampleRateHint() const
{
	int32 Rate = PythonAudioPrivate::MinSampleRateHint;
	if (Source == EPythonAudioSource::Submix && GEngine)
	{
		const FAudioDeviceHandle Device = GEngine->GetMainAudioDevice();
		if (Device.IsValid())
		{
			Rate = FMath::Max(Rate, (int32)Device->GetSampleRate());
		}
	}
	return Rate;
}

int64 UPythonAudioStreamComponent::GetFrameBytes() const
{
	const int64 Frames = FMath::CeilToInt64(ChunkSeconds * SampleRateHint());
	return Frames * (bDownmixToMono ? 1 : PythonAudioPrivate::MaxChannels) * (int64)sizeof(float);
}

TSharedPtr<FJsonObject> UPythonAudioStreamComponent::MakeStreamInfo() const
{
	TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
	Info->SetStringField(TEXT("source"), Source == EPythonAudioSource::Microphone ? TEXT("microphone") : Source == EPythonAudioSource::Submix ? TEXT("submix") : TEXT("manual"));
	Info->SetNumberField(TEXT("chunk_seconds"), ChunkSeconds);
	return Info;
}

void UPythonAudioStreamComponent::OnStreamOpened()
{
	Chunker = MakeShared<FChunker, ESPMode::ThreadSafe>(Stream, ChunkSeconds, bDownmixToMono);
	if (bCaptureOnOpen && Source != EPythonAudioSource::Manual)
	{
		StartCapture();
	}
}

void UPythonAudioStreamComponent::OnStreamClosing()
{
	StopCapture();
	if (Chunker.IsValid())
	{
		Chunker->Detach(); //A late audio callback finds nothing to push to
		Chunker.Reset();
	}
}

bool UPythonAudioStreamComponent::StartCapture()
{
	if (bCapturing)
	{
		return true;
	}
	if (!Chunker.IsValid() && !OpenStream())
	{
		return false;
	}
	if (Source == EPythonAudioSource::Microphone)
	{
		Microphone = MakeShared<Audio::FAudioCapture>();
		Audio::FAudioCaptureDeviceParams Params;
		TSharedPtr<FChunker, ESPMode::ThreadSafe> Target = Chunker;
		const bool bOpened = Microphone->OpenAudioCaptureStream(Params,
			[Target](const void* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double, bool)
			{
				Target->Add(static_cast<const float*>(InAudio), NumFrames, NumChannels, SampleRate);
			}, 1024);
		if (!bOpened || !Microphone->StartStream())
		{
			UE_LOG(LogPythonCLI, Warning, TEXT("Python stream '%s': could not open the microphone (is the AudioCapture plugin enabled?)"), *StreamName);
			Microphone.Reset();
			return false;
		}
	}
	else if (Source == EPythonAudioSource::Submix)
	{
		UWorld* World = GetWorld();
		FAudioDeviceHandle Device = World ? World->GetAudioDevice() : FAudioDeviceHandle();
		if (!Device.IsValid() && GEngine)
		{
			Device = GEngine->GetMainAudioDevice();
		}
		if (!Device.IsValid())
		{
			UE_LOG(LogPythonCLI, Warning, TEXT("Python stream '%s': no audio device to listen to"), *StreamName);
			return false;
		}
		USoundSubmix* Target = Submix ? Submix.Get() : &Device->GetMainSubmixObject();
		Listener = MakeShared<FSubmixListener, ESPMode::ThreadSafe>(Chunker);
		Device->RegisterSubmixBufferListener(Listener.ToSharedRef(), *Target);
		ListeningTo = Target;
	}
	bCapturing = true;
	return true;
}

void UPythonAudioStreamComponent::StopCapture()
{
	if (Microphone)
	{
		Microphone->StopStream();
		Microphone->CloseStream();
		Microphone.Reset();
	}
	if (Listener.IsValid())
	{
		UWorld* World = GetWorld();
		FAudioDeviceHandle Device = World ? World->GetAudioDevice() : FAudioDeviceHandle();
		if (!Device.IsValid() && GEngine)
		{
			Device = GEngine->GetMainAudioDevice();
		}
		if (Device.IsValid() && ListeningTo.IsValid())
		{
			Device->UnregisterSubmixBufferListener(Listener.ToSharedRef(), *ListeningTo.Get());
		}
		Listener.Reset();
		ListeningTo.Reset();
	}
	bCapturing = false;
}

void UPythonAudioStreamComponent::PushAudio(const TArray<float>& Samples, int32 NumChannels, int32 SampleRate)
{
	if (NumChannels > 0)
	{
		PushAudioSamples(Samples.GetData(), Samples.Num() / NumChannels, NumChannels, SampleRate);
	}
}

void UPythonAudioStreamComponent::PushAudioSamples(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate)
{
	if (TSharedPtr<FChunker, ESPMode::ThreadSafe> Target = Chunker)
	{
		Target->Add(Interleaved, NumFrames, NumChannels, SampleRate);
	}
}
