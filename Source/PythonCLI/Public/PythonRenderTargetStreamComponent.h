#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"
#include "PythonStreamComponent.h"
#include "PythonRenderTargetStreamComponent.generated.h"

class UTextureRenderTarget2D;
class USceneCaptureComponent2D;

/**
 * Streams a render target (for example a Scene Capture 2D's Texture Target) to Python with asynchronous GPU
 * readback: no ReadPixels stall. The pixels land straight in a shared-memory slot, about two frames after
 * the capture, and Python gets an (H, W, C) array: uint8 BGRA for RTF_RGBA8 render targets (frame.rgb()
 * gives RGB), float16 RGBA for RTF_RGBA16f, float32 for RTF_R32f, ...
 */
UCLASS(ClassGroup = "Python", meta = (BlueprintSpawnableComponent))
class PYTHONCLI_API UPythonRenderTargetStreamComponent : public UPythonStreamComponent
{
	GENERATED_BODY()
public:
	UPythonRenderTargetStreamComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	TObjectPtr<UTextureRenderTarget2D> RenderTarget;

	/** Used when RenderTarget is unset: stream this scene capture's Texture Target. */
	UPROPERTY(BlueprintReadWrite, Category = "Python Stream")
	TObjectPtr<USceneCaptureComponent2D> SceneCapture;

	/** Capture while the component ticks; otherwise call Capture(). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	bool bAutoCapture = true;

	/** Automatic captures per second; 0 = every frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream", meta = (ClampMin = "0"))
	float CapturesPerSecond = 0.f;

	/** Start reading the render target back now. The frame reaches Python when the GPU copy is done. Never
	 waits: false (a dropped frame) while every slot is in use, or if there is nothing to capture. */
	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	bool Capture();

	/** Hand finished readbacks to Python (done every tick; call it yourself when not ticking). */
	void PollReadbacks();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** dtype, channels and layout ("BGRA", "RGBA", "RG", "R") for a pixel format; false if unsupported. */
	static bool DescribePixelFormat(EPixelFormat Format, EPyDType& OutDType, int32& OutChannels, FString& OutLayout);

protected:
	virtual int64 GetFrameBytes() const override;
	virtual TSharedPtr<FJsonObject> MakeStreamInfo() const override;
	virtual void OnStreamOpened() override;
	virtual void OnStreamClosing() override;

private:
	UTextureRenderTarget2D* GetTarget() const;

	struct FRenderState;
	TSharedPtr<FRenderState, ESPMode::ThreadSafe> RenderState;
	FIntPoint OpenedSize = FIntPoint::ZeroValue;
	EPixelFormat OpenedFormat = PF_Unknown;
	double LastCaptureTime = 0.0;
	bool bWarnedFormat = false;
};
