#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PythonTypes.h"
#include "PythonImageLibrary.generated.h"

class UTexture2D;
class UTextureRenderTarget2D;

UENUM(BlueprintType)
enum class EPyImageChannels : uint8
{
	/** One channel: (R + G + B) / 3. */
	Grayscale,
	RGB,
	RGBA,
};

UENUM(BlueprintType)
enum class EPyImageNormalization : uint8
{
	/** 0..1 */
	ZeroToOne,
	/** -1..1 */
	MinusOneToOne,
	/** 0..255 */
	Raw,
	/** (x - mean) / std with ImageNet's RGB statistics (torchvision's pretrained models). */
	ImageNet,
};

/** How an image becomes a float32 tensor. */
USTRUCT(BlueprintType)
struct PYTHONCLI_API FPyImageOptions
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Image")
	EPyImageChannels Channels = EPyImageChannels::RGB;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Image")
	EPyImageNormalization Normalization = EPyImageNormalization::ZeroToOne;

	/** Resize (bilinear) to this size first; 0 keeps the image's size. E.g. 28 x 28 for MNIST. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Image")
	FIntPoint Size = FIntPoint::ZeroValue;

	/** 1 - value (before scaling), e.g. dark-on-light drawings for MNIST's light-on-dark digits. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Image")
	bool bInvert = false;

	/** (C, H, W) instead of (H, W, C): PyTorch's layout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Image")
	bool bChannelsFirst = false;
};

/**
 * Images <-> tensors. Textures are read from the editor's source data, a CPU copy (texture Availability: CPU),
 * or uncompressed platform data (e.g. Compression: UserInterface2D or Grayscale); render targets with a
 * blocking read (use a Python Render Target Stream for every frame). Tensors go back to transient textures
 * or straight into render targets.
 */
UCLASS()
class PYTHONCLI_API UPythonImageLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** Image -> float32 tensor ((H, W, C), or (C, H, W)). */
	UFUNCTION(BlueprintPure, Category = "Python|Image")
	static FPyTensor TextureToTensor(UTexture2D* Texture, const FPyImageOptions& Options);

	/** As TextureToTensor; reads the render target now (blocking). */
	UFUNCTION(BlueprintCallable, Category = "Python|Image")
	static FPyTensor RenderTargetToTensor(UTextureRenderTarget2D* RenderTarget, const FPyImageOptions& Options);

	/** A (H, W), (H, W, 1/3/4) or (C, H, W) tensor -> a new transient texture. Float values are taken as 0..1,
	 uint8 as 0..255. */
	UFUNCTION(BlueprintCallable, Category = "Python|Image")
	static UTexture2D* TensorToTexture(const FPyTensor& Tensor, bool bChannelsFirst = false);

	/** Write an image tensor into an existing texture made by TensorToTexture (same size): no new texture per
	 frame. False if the sizes differ. */
	UFUNCTION(BlueprintCallable, Category = "Python|Image")
	static bool UpdateTextureFromTensor(UTexture2D* Texture, const FPyTensor& Tensor, bool bChannelsFirst = false);

	/** Write an image tensor into a render target (resized to the tensor's size if needed), on the render
	 thread: RGBA8, RGBA16f and RGBA32f targets. Float values 0..1 (stored as they are in float targets),
	 uint8 0..255. */
	UFUNCTION(BlueprintCallable, Category = "Python|Image")
	static bool WriteTensorToRenderTarget(const FPyTensor& Tensor, UTextureRenderTarget2D* RenderTarget, bool bChannelsFirst = false);

	/** C++: a texture's pixels as linear 0..1 values of its stored data (no gamma conversion). False (with
	 the reason) if the texture can't be read on the CPU. */
	static bool ReadTexture(UTexture2D* Texture, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError);
	static bool ReadRenderTarget(UTextureRenderTarget2D* RenderTarget, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError);
	static FPyTensor PixelsToTensor(const TArray<FLinearColor>& Pixels, int32 Width, int32 Height, const FPyImageOptions& Options);
	/** An image tensor's pixels (row-major, RGBA; 1 or 3 channels fill the rest). False with the reason if it
	 isn't an image. */
	static bool TensorToPixels(const FPyTensor& Tensor, bool bChannelsFirst, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError);
	/** A transient BGRA8 texture from pixels. */
	static UTexture2D* MakeTexture(const TArray<FLinearColor>& Pixels, int32 Width, int32 Height);
};
