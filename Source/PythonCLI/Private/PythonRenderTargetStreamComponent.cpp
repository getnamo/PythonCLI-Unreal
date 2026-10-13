#include "PythonRenderTargetStreamComponent.h"
#include "PythonCLI.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Dom/JsonObject.h"
#include "Engine/TextureRenderTarget2D.h"
#include "HAL/PlatformTime.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include <atomic>

struct UPythonRenderTargetStreamComponent::FRenderState
{
	struct FPending
	{
		int32 Slot = INDEX_NONE;
		int32 Width = 0;
		int32 Height = 0;
		int32 BytesPerPixel = 0;
		FPythonStreamFrame Frame;
	};

	TSharedPtr<FPythonStream, ESPMode::ThreadSafe> Stream;
	/** One per slot: a slot and its readback are acquired together. */
	TArray<TUniquePtr<FRHIGPUTextureReadback>> Readbacks;
	/** Render thread only: captures in order, waiting for the GPU. */
	TArray<FPending> InFlight;
	/** Captures not yet handed to Python (game thread reads it to decide whether to poll). */
	std::atomic<int32> Outstanding{ 0 };
	std::atomic<bool> bPollQueued{ false };
};

UPythonRenderTargetStreamComponent::UPythonRenderTargetStreamComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	StreamName = TEXT("camera");
	Policy = EPythonStreamPolicy::Latest;
	Slots = 3;
}

bool UPythonRenderTargetStreamComponent::DescribePixelFormat(EPixelFormat Format, EPyDType& OutDType, int32& OutChannels, FString& OutLayout)
{
	switch (Format)
	{
	case PF_B8G8R8A8: OutDType = EPyDType::UInt8; OutChannels = 4; OutLayout = TEXT("BGRA"); return true;
	case PF_R8G8B8A8: OutDType = EPyDType::UInt8; OutChannels = 4; OutLayout = TEXT("RGBA"); return true;
	case PF_FloatRGBA: OutDType = EPyDType::Float16; OutChannels = 4; OutLayout = TEXT("RGBA"); return true;
	case PF_A32B32G32R32F: OutDType = EPyDType::Float32; OutChannels = 4; OutLayout = TEXT("RGBA"); return true;
	case PF_G16R16F: OutDType = EPyDType::Float16; OutChannels = 2; OutLayout = TEXT("RG"); return true;
	case PF_G32R32F: OutDType = EPyDType::Float32; OutChannels = 2; OutLayout = TEXT("RG"); return true;
	case PF_R16F: OutDType = EPyDType::Float16; OutChannels = 1; OutLayout = TEXT("R"); return true;
	case PF_R32_FLOAT: OutDType = EPyDType::Float32; OutChannels = 1; OutLayout = TEXT("R"); return true;
	case PF_G8: case PF_R8: OutDType = EPyDType::UInt8; OutChannels = 1; OutLayout = TEXT("R"); return true;
	default: return false;
	}
}

UTextureRenderTarget2D* UPythonRenderTargetStreamComponent::GetTarget() const
{
	if (RenderTarget)
	{
		return RenderTarget;
	}
	return SceneCapture ? SceneCapture->TextureTarget.Get() : nullptr;
}

int64 UPythonRenderTargetStreamComponent::GetFrameBytes() const
{
	EPyDType DType;
	int32 Channels = 0;
	FString Layout;
	const UTextureRenderTarget2D* RT = GetTarget();
	if (!RT || !DescribePixelFormat(RT->GetFormat(), DType, Channels, Layout))
	{
		return 4; //Opened for real at the first Capture()
	}
	return (int64)RT->SizeX * RT->SizeY * Channels * FPyTensor::ItemSize(DType);
}

TSharedPtr<FJsonObject> UPythonRenderTargetStreamComponent::MakeStreamInfo() const
{
	TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
	EPyDType DType;
	int32 Channels = 0;
	FString Layout;
	if (const UTextureRenderTarget2D* RT = GetTarget())
	{
		if (DescribePixelFormat(RT->GetFormat(), DType, Channels, Layout))
		{
			Info->SetNumberField(TEXT("width"), RT->SizeX);
			Info->SetNumberField(TEXT("height"), RT->SizeY);
			Info->SetNumberField(TEXT("channels"), Channels);
			Info->SetStringField(TEXT("dtype"), FPyTensor::DTypeName(DType));
			Info->SetStringField(TEXT("layout"), Layout);
		}
		Info->SetStringField(TEXT("pixel_format"), GetPixelFormatString(RT->GetFormat()));
	}
	return Info;
}

void UPythonRenderTargetStreamComponent::OnStreamOpened()
{
	const UTextureRenderTarget2D* RT = GetTarget();
	OpenedSize = RT ? FIntPoint(RT->SizeX, RT->SizeY) : FIntPoint::ZeroValue;
	OpenedFormat = RT ? RT->GetFormat() : PF_Unknown;
	RenderState = MakeShared<FRenderState, ESPMode::ThreadSafe>();
	RenderState->Stream = Stream;
	for (int32 i = 0; i < Stream->GetConfig().Slots; ++i)
	{
		RenderState->Readbacks.Add(MakeUnique<FRHIGPUTextureReadback>(TEXT("PythonStreamReadback")));
	}
}

void UPythonRenderTargetStreamComponent::OnStreamClosing()
{
	if (!RenderState.IsValid())
	{
		return;
	}
	ENQUEUE_RENDER_COMMAND(PythonStreamRelease)([State = RenderState](FRHICommandListImmediate&)
	{
		for (const FRenderState::FPending& P : State->InFlight)
		{
			State->Stream->Cancel(P.Slot);
		}
		State->InFlight.Reset();
		State->Readbacks.Reset();
		State->Outstanding = 0;
	});
	FlushRenderingCommands(); //Nothing on the render thread touches the stream's memory after this
	RenderState.Reset();
}

bool UPythonRenderTargetStreamComponent::Capture()
{
	UTextureRenderTarget2D* RT = GetTarget();
	if (!RT)
	{
		return false;
	}
	EPyDType DType;
	int32 Channels = 0;
	FString Layout;
	const EPixelFormat Format = RT->GetFormat();
	if (!DescribePixelFormat(Format, DType, Channels, Layout))
	{
		if (!bWarnedFormat)
		{
			bWarnedFormat = true;
			UE_LOG(LogPythonCLI, Warning, TEXT("Python stream '%s': render target format %s isn't supported (use RGBA8, RGBA16f, RGBA32f, R32f, R16f, RG16f, RG32f or R8)"),
				*StreamName, GetPixelFormatString(Format));
		}
		return false;
	}
	const FIntPoint Size(RT->SizeX, RT->SizeY);
	if (!IsStreamOpen() || Size != OpenedSize || Format != OpenedFormat)
	{
		CloseStream(); //New size or format: new slots
		if (!OpenStream())
		{
			return false;
		}
	}
	FTextureRenderTargetResource* Resource = RT->GameThread_GetRenderTargetResource();
	if (!Resource || !RenderState.IsValid())
	{
		return false;
	}
	//Never waits: slots come back through PollReadbacks on later ticks, so a busy stream skips this capture
	const int32 Slot = Stream->AcquireSlot(0.0);
	if (Slot == INDEX_NONE)
	{
		return false;
	}
	LastCaptureTime = FPlatformTime::Seconds();

	FRenderState::FPending P;
	P.Slot = Slot;
	P.Width = Size.X;
	P.Height = Size.Y;
	P.BytesPerPixel = Channels * FPyTensor::ItemSize(DType);
	P.Frame.DType = DType;
	P.Frame.Shape = { Size.Y, Size.X, Channels };
	P.Frame.NumBytes = (int64)Size.X * Size.Y * P.BytesPerPixel;
	P.Frame.Layout = Layout;
	P.Frame.Time = LastCaptureTime;
	P.Frame.FrameNumber = GFrameCounter;
	++RenderState->Outstanding;
	ENQUEUE_RENDER_COMMAND(PythonStreamReadback)([State = RenderState, Resource, P = MoveTemp(P)](FRHICommandListImmediate& RHICmdList) mutable
	{
		FRHITexture* Texture = Resource->GetRenderTargetTexture();
		if (!Texture || !State->Readbacks.IsValidIndex(P.Slot))
		{
			State->Stream->Cancel(P.Slot);
			--State->Outstanding;
			return;
		}
		FRDGBuilder GraphBuilder(RHICmdList);
		AddEnqueueCopyPass(GraphBuilder, State->Readbacks[P.Slot].Get(), RegisterExternalTexture(GraphBuilder, Texture, TEXT("PythonStreamTarget")));
		GraphBuilder.Execute();
		State->InFlight.Add(MoveTemp(P));
	});
	return true;
}

void UPythonRenderTargetStreamComponent::PollReadbacks()
{
	if (!RenderState.IsValid() || RenderState->Outstanding.load() == 0 || RenderState->bPollQueued.exchange(true))
	{
		return;
	}
	ENQUEUE_RENDER_COMMAND(PythonStreamPoll)([State = RenderState](FRHICommandListImmediate&)
	{
		State->bPollQueued = false;
		while (State->InFlight.Num() > 0) //In capture order
		{
			FRenderState::FPending& P = State->InFlight[0];
			FRHIGPUTextureReadback* Readback = State->Readbacks[P.Slot].Get();
			if (!Readback->IsReady())
			{
				break;
			}
			int32 PitchPixels = 0;
			const uint8* Src = static_cast<const uint8*>(Readback->Lock(PitchPixels));
			uint8* Dst = State->Stream->GetSlotData(P.Slot);
			if (Src && Dst)
			{
				const int64 RowBytes = (int64)P.Width * P.BytesPerPixel;
				const int64 SrcPitch = (int64)FMath::Max(PitchPixels, P.Width) * P.BytesPerPixel;
				if (SrcPitch == RowBytes)
				{
					FMemory::Memcpy(Dst, Src, RowBytes * P.Height);
				}
				else
				{
					for (int32 Y = 0; Y < P.Height; ++Y)
					{
						FMemory::Memcpy(Dst + Y * RowBytes, Src + Y * SrcPitch, RowBytes);
					}
				}
			}
			if (Src)
			{
				Readback->Unlock();
			}
			if (Src && Dst)
			{
				State->Stream->Commit(P.Slot, P.Frame);
			}
			else
			{
				State->Stream->Cancel(P.Slot);
			}
			--State->Outstanding;
			State->InFlight.RemoveAt(0, EAllowShrinking::No);
		}
	});
}

void UPythonRenderTargetStreamComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (bAutoCapture && GetTarget() && (CapturesPerSecond <= 0.f || FPlatformTime::Seconds() - LastCaptureTime >= 1.0 / CapturesPerSecond))
	{
		Capture();
	}
	PollReadbacks();
}
