#pragma once

#include "CoreMinimal.h"
#include "PythonStreamComponent.h"
#include "PythonAudioStreamComponent.generated.h"

class USoundSubmix;
namespace Audio { class FAudioCapture; }

UENUM(BlueprintType)
enum class EPythonAudioSource : uint8
{
	/** The default capture device (needs the AudioCapture plugin). */
	Microphone,
	/** What a submix renders (the game's audio; default: the main output). */
	Submix,
	/** Only what you pass to PushAudio. */
	Manual,
};

/**
 * Streams audio to Python as float32 chunks of ChunkSeconds: frame.array is (frames, channels) and
 * frame.meta["sample_rate"] the rate. Audio is never blocked: when Python falls behind every slot, chunks
 * are dropped (counted in the stream stats) rather than stalling the audio thread.
 */
UCLASS(ClassGroup = "Python", meta = (BlueprintSpawnableComponent))
class PYTHONCLI_API UPythonAudioStreamComponent : public UPythonStreamComponent
{
	GENERATED_BODY()
public:
	UPythonAudioStreamComponent();
	virtual ~UPythonAudioStreamComponent() override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	EPythonAudioSource Source = EPythonAudioSource::Microphone;

	/** Submix source: the submix to listen to. Unset: the main output submix. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	TObjectPtr<USoundSubmix> Submix;

	/** Audio per frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream", meta = (ClampMin = "0.005", ClampMax = "1.0"))
	float ChunkSeconds = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	bool bDownmixToMono = false;

	/** Start capturing as soon as the stream opens. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	bool bCaptureOnOpen = true;

	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	bool StartCapture();

	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	void StopCapture();

	UFUNCTION(BlueprintPure, Category = "Python Stream")
	bool IsCapturing() const { return bCapturing; }

	/** Stream interleaved samples (any source; the Manual source only gets these). */
	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	void PushAudio(const TArray<float>& Samples, int32 NumChannels = 1, int32 SampleRate = 48000);

	/** C++, any thread while the stream is open. */
	void PushAudioSamples(const float* Interleaved, int32 NumFrames, int32 NumChannels, int32 SampleRate);

protected:
	virtual int64 GetFrameBytes() const override;
	virtual TSharedPtr<FJsonObject> MakeStreamInfo() const override;
	virtual void OnStreamOpened() override;
	virtual void OnStreamClosing() override;

private:
	class FChunker;
	class FSubmixListener;

	int32 SampleRateHint() const;

	TSharedPtr<FChunker, ESPMode::ThreadSafe> Chunker;
	TSharedPtr<Audio::FAudioCapture> Microphone; //Shared: its deleter is created where the type is complete
	TSharedPtr<FSubmixListener, ESPMode::ThreadSafe> Listener;
	TWeakObjectPtr<USoundSubmix> ListeningTo;
	bool bCapturing = false;
};
