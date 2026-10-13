#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PythonStream.h"
#include "PythonTypes.h"
#include "PythonStreamComponent.generated.h"

class UPythonComponent;

/**
 * Streams frames to a Python script: @ue.on_stream(StreamName) handlers, or `for frame in ue.stream(...)`.
 * Frames go to the script of PythonComponent (by default the first Python component on the same actor).
 * This base class streams tensors you push (PushTensor, or StepTensor for Lockstep); the subclasses stream
 * observations, render targets and audio.
 */
UCLASS(ClassGroup = "Python", meta = (BlueprintSpawnableComponent))
class PYTHONCLI_API UPythonStreamComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UPythonStreamComponent();

	/** What the script handles: @ue.on_stream("StreamName"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	FString StreamName = TEXT("stream");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	EPythonStreamPolicy Policy = EPythonStreamPolicy::Latest;

	/** Frames in flight at once; each is a slot of memory that comes back when Python is done with it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream", meta = (ClampMin = "1", ClampMax = "64"))
	int32 Slots = 3;

	/** Queue policy: how long a producer waits for a free slot before dropping the frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream", meta = (ClampMin = "0"))
	float MaxWaitSeconds = 1.f;

	/** Lockstep: how long a step waits for the Python handler's result. The game thread waits with it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream", meta = (ClampMin = "0"))
	float StepTimeoutSeconds = 10.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream", AdvancedDisplay)
	EPythonStreamTransport Transport = EPythonStreamTransport::Auto;

	/** PushTensor / StepTensor: the largest frame in bytes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream", meta = (ClampMin = "1"))
	int32 MaxFrameBytes = 65536;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	bool bOpenOnBeginPlay = true;

	/** Whose script gets the frames. Unset: the first Python component on this actor. */
	UPROPERTY(BlueprintReadWrite, Category = "Python Stream")
	TObjectPtr<UPythonComponent> PythonComponent;

	/** Tell Python about the stream (and create its shared memory). Frames before the script runs are
	 delivered once it has started. */
	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	bool OpenStream();

	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	void CloseStream();

	UFUNCTION(BlueprintPure, Category = "Python Stream")
	bool IsStreamOpen() const;

	/** Send a tensor as one frame (frame.array in Python; MetaJson becomes frame.meta). False if it was
	 dropped: no free slot, or no running Python process. */
	UFUNCTION(BlueprintCallable, Category = "Python Stream", meta = (AutoCreateRefTerm = "MetaJson"))
	bool PushTensor(const FPyTensor& Tensor, const FString& MetaJson);

	/** Lockstep: send a tensor and wait for the handler's return value (Result.JsonResult / Result.Tensors). */
	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	bool StepTensor(const FPyTensor& Tensor, FPythonCallResult& Result);

	UFUNCTION(BlueprintPure, Category = "Python Stream")
	FPythonStreamStats GetStreamStats() const;

	/** C++: the open stream; its producer API works from any thread. */
	TSharedPtr<FPythonStream, ESPMode::ThreadSafe> GetStream() const { return Stream; }

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	/** Bytes per slot. */
	virtual int64 GetFrameBytes() const { return MaxFrameBytes; }
	/** frame.info in Python. */
	virtual TSharedPtr<FJsonObject> MakeStreamInfo() const { return nullptr; }
	virtual void OnStreamOpened() {}
	/** Stop producers before the stream closes. */
	virtual void OnStreamClosing() {}

	UPythonComponent* ResolvePythonComponent();

	TSharedPtr<FPythonStream, ESPMode::ThreadSafe> Stream;
};
