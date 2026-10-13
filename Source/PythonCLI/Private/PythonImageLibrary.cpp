#include "PythonImageLibrary.h"
#include "PythonCLI.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageCore.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "TextureResource.h"
#include "Math/Float16Color.h"

namespace PythonImagePrivate
{
	static FLinearColor FromBytes(const FColor& C)
	{
		return FLinearColor(C.R / 255.f, C.G / 255.f, C.B / 255.f, C.A / 255.f);
	}

	static uint8 ToByte(float Value)
	{
		return (uint8)FMath::Clamp(FMath::RoundToInt(Value * 255.f), 0, 255);
	}

	static FLinearColor Sample(const TArray<FLinearColor>& Pixels, int32 Width, int32 Height, float X, float Y)
	{
		const float Fx = FMath::Clamp(X, 0.f, (float)(Width - 1));
		const float Fy = FMath::Clamp(Y, 0.f, (float)(Height - 1));
		const int32 X0 = FMath::FloorToInt(Fx), Y0 = FMath::FloorToInt(Fy);
		const int32 X1 = FMath::Min(X0 + 1, Width - 1), Y1 = FMath::Min(Y0 + 1, Height - 1);
		const float Tx = Fx - X0, Ty = Fy - Y0;
		const FLinearColor Top = FMath::Lerp(Pixels[Y0 * Width + X0], Pixels[Y0 * Width + X1], Tx);
		const FLinearColor Bottom = FMath::Lerp(Pixels[Y1 * Width + X0], Pixels[Y1 * Width + X1], Tx);
		return FMath::Lerp(Top, Bottom, Ty);
	}

	static const float ImageNetMean[3] = { 0.485f, 0.456f, 0.406f };
	static const float ImageNetStd[3] = { 0.229f, 0.224f, 0.225f };
}

bool UPythonImageLibrary::ReadTexture(UTexture2D* Texture, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
{
	using namespace PythonImagePrivate;
	OutPixels.Reset();
	if (!Texture)
	{
		OutError = TEXT("no texture");
		return false;
	}

	//1. The editor's source image, 2. a CPU copy (Availability: CPU). Both as the stored 8-bit values.
	FImage Image;
	bool bImage = false;
#if WITH_EDITORONLY_DATA
	bImage = Texture->Source.IsValid() && Texture->Source.GetMipImage(Image, 0, 0, 0);
#endif
	if (!bImage)
	{
		const FSharedImageConstRef Copy = Texture->GetCPUCopy();
		if (Copy.IsValid())
		{
			Copy->CopyTo(Image);
			bImage = true;
		}
	}
	if (bImage)
	{
		FImage Bytes;
		Image.CopyTo(Bytes, ERawImageFormat::BGRA8, Image.GammaSpace == EGammaSpace::Linear ? EGammaSpace::Linear : EGammaSpace::sRGB);
		OutWidth = (int32)Bytes.SizeX;
		OutHeight = (int32)Bytes.SizeY;
		const TArrayView64<FColor> Colors = Bytes.AsBGRA8();
		OutPixels.Reserve(Colors.Num());
		for (const FColor& C : Colors)
		{
			OutPixels.Add(FromBytes(C));
		}
		return true;
	}

	//3. Uncompressed platform data still in memory (e.g. transient textures, UserInterface2D / Grayscale)
	FTexturePlatformData* Data = Texture->GetPlatformData();
	if (Data && Data->Mips.Num() > 0)
	{
		FTexture2DMipMap& Mip = Data->Mips[0];
		const EPixelFormat Format = Data->PixelFormat;
		const int32 Bpp = (Format == PF_B8G8R8A8 || Format == PF_R8G8B8A8) ? 4 : (Format == PF_G8 ? 1 : 0);
		if (Bpp > 0 && Mip.BulkData.GetBulkDataSize() >= (int64)Mip.SizeX * Mip.SizeY * Bpp)
		{
			const uint8* Src = static_cast<const uint8*>(Mip.BulkData.Lock(LOCK_READ_ONLY));
			if (Src)
			{
				OutWidth = Mip.SizeX;
				OutHeight = Mip.SizeY;
				OutPixels.SetNumUninitialized(OutWidth * OutHeight);
				for (int32 i = 0; i < OutPixels.Num(); ++i)
				{
					const uint8* P = Src + (int64)i * Bpp;
					OutPixels[i] = Bpp == 1 ? FromBytes(FColor(P[0], P[0], P[0], 255))
						: Format == PF_B8G8R8A8 ? FromBytes(FColor(P[2], P[1], P[0], P[3])) : FromBytes(FColor(P[0], P[1], P[2], P[3]));
				}
			}
			Mip.BulkData.Unlock();
			if (Src)
			{
				return true;
			}
		}
	}
	OutError = FString::Printf(TEXT("texture %s can't be read on the CPU: set its Availability to CPU, or its Compression to UserInterface2D or Grayscale (uncompressed), or render it to a render target"),
		*Texture->GetName());
	return false;
}

bool UPythonImageLibrary::ReadRenderTarget(UTextureRenderTarget2D* RenderTarget, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
{
	using namespace PythonImagePrivate;
	OutPixels.Reset();
	FTextureRenderTargetResource* Resource = RenderTarget ? RenderTarget->GameThread_GetRenderTargetResource() : nullptr;
	if (!Resource)
	{
		OutError = TEXT("no render target (or it has no resource yet)");
		return false;
	}
	OutWidth = RenderTarget->SizeX;
	OutHeight = RenderTarget->SizeY;
	const EPixelFormat Format = RenderTarget->GetFormat();
	if (Format == PF_B8G8R8A8 || Format == PF_R8G8B8A8)
	{
		TArray<FColor> Colors;
		if (!Resource->ReadPixels(Colors))
		{
			OutError = TEXT("reading the render target failed");
			return false;
		}
		OutPixels.Reserve(Colors.Num());
		for (const FColor& C : Colors)
		{
			OutPixels.Add(FromBytes(C));
		}
		return true;
	}
	if (!Resource->ReadLinearColorPixels(OutPixels))
	{
		OutError = TEXT("reading the render target failed");
		return false;
	}
	return true;
}

FPyTensor UPythonImageLibrary::PixelsToTensor(const TArray<FLinearColor>& Pixels, int32 Width, int32 Height, const FPyImageOptions& Options)
{
	using namespace PythonImagePrivate;
	const int32 OutW = Options.Size.X > 0 ? Options.Size.X : Width;
	const int32 OutH = Options.Size.Y > 0 ? Options.Size.Y : Height;
	const int32 C = Options.Channels == EPyImageChannels::Grayscale ? 1 : Options.Channels == EPyImageChannels::RGB ? 3 : 4;
	TArray<float> Values;
	if (Pixels.Num() != Width * Height || OutW <= 0 || OutH <= 0)
	{
		return FPyTensor();
	}
	Values.SetNumUninitialized(OutW * OutH * C);
	const bool bResize = OutW != Width || OutH != Height;
	for (int32 Y = 0; Y < OutH; ++Y)
	{
		for (int32 X = 0; X < OutW; ++X)
		{
			//Pixel centers map onto the source
			const FLinearColor P = bResize ? Sample(Pixels, Width, Height, (X + 0.5f) * Width / OutW - 0.5f, (Y + 0.5f) * Height / OutH - 0.5f) : Pixels[Y * Width + X];
			float Ch[4] = { P.R, P.G, P.B, P.A };
			if (C == 1)
			{
				Ch[0] = (P.R + P.G + P.B) / 3.f;
			}
			for (int32 k = 0; k < C; ++k)
			{
				float V = Ch[k];
				if (Options.bInvert && k < 3)
				{
					V = 1.f - V;
				}
				switch (Options.Normalization)
				{
				case EPyImageNormalization::MinusOneToOne: V = V * 2.f - 1.f; break;
				case EPyImageNormalization::Raw: V = V * 255.f; break;
				case EPyImageNormalization::ImageNet:
					if (C == 1)
					{
						V = (V - 0.449f) / 0.226f; //The mean of the RGB statistics
					}
					else if (k < 3)
					{
						V = (V - ImageNetMean[k]) / ImageNetStd[k];
					}
					break;
				default: break;
				}
				const int32 Index = Options.bChannelsFirst ? (k * OutH + Y) * OutW + X : (Y * OutW + X) * C + k;
				Values[Index] = V;
			}
		}
	}
	return FPyTensor::FromFloats(Values, Options.bChannelsFirst ? TArray<int64>{ C, OutH, OutW } : TArray<int64>{ OutH, OutW, C });
}

FPyTensor UPythonImageLibrary::TextureToTensor(UTexture2D* Texture, const FPyImageOptions& Options)
{
	TArray<FLinearColor> Pixels;
	int32 W = 0, H = 0;
	FString Error;
	if (!ReadTexture(Texture, Pixels, W, H, Error))
	{
		UE_LOG(LogPythonCLI, Warning, TEXT("TextureToTensor: %s"), *Error);
		return FPyTensor();
	}
	return PixelsToTensor(Pixels, W, H, Options);
}

FPyTensor UPythonImageLibrary::RenderTargetToTensor(UTextureRenderTarget2D* RenderTarget, const FPyImageOptions& Options)
{
	TArray<FLinearColor> Pixels;
	int32 W = 0, H = 0;
	FString Error;
	if (!ReadRenderTarget(RenderTarget, Pixels, W, H, Error))
	{
		UE_LOG(LogPythonCLI, Warning, TEXT("RenderTargetToTensor: %s"), *Error);
		return FPyTensor();
	}
	return PixelsToTensor(Pixels, W, H, Options);
}

bool UPythonImageLibrary::TensorToPixels(const FPyTensor& Tensor, bool bChannelsFirst, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight, FString& OutError)
{
	OutPixels.Reset();
	TArray<int64> Shape = Tensor.Shape;
	while (Shape.Num() > 3 && Shape[0] == 1)
	{
		Shape.RemoveAt(0); //A batch of one
	}
	if (!Tensor.IsValid() || Shape.Num() < 2 || Shape.Num() > 3 || Tensor.DType == EPyDType::Float16)
	{
		OutError = TEXT("expected a (H, W), (H, W, C) or (C, H, W) tensor (not float16)");
		return false;
	}
	int32 H = 0, W = 0, C = 1;
	if (Shape.Num() == 2)
	{
		H = (int32)Shape[0];
		W = (int32)Shape[1];
	}
	else if (bChannelsFirst)
	{
		C = (int32)Shape[0];
		H = (int32)Shape[1];
		W = (int32)Shape[2];
	}
	else
	{
		H = (int32)Shape[0];
		W = (int32)Shape[1];
		C = (int32)Shape[2];
	}
	if (C != 1 && C != 3 && C != 4)
	{
		OutError = FString::Printf(TEXT("%d channels (1, 3 or 4 expected)"), C);
		return false;
	}
	const TArray<float> Values = Tensor.ToFloats();
	const float Scale = Tensor.DType == EPyDType::UInt8 ? 1.f / 255.f : 1.f;
	OutPixels.SetNumUninitialized(W * H);
	for (int32 Pixel = 0; Pixel < W * H; ++Pixel)
	{
		const int32 Y = Pixel / W, X = Pixel % W;
		auto At = [&](int32 k) -> float
		{
			const int32 Index = (C == 1 || !bChannelsFirst) ? Pixel * C + k : (k * H + Y) * W + X;
			return Values[Index] * Scale;
		};
		OutPixels[Pixel] = C == 1 ? FLinearColor(At(0), At(0), At(0), 1.f) : FLinearColor(At(0), At(1), At(2), C == 4 ? At(3) : 1.f);
	}
	OutWidth = W;
	OutHeight = H;
	return true;
}

UTexture2D* UPythonImageLibrary::MakeTexture(const TArray<FLinearColor>& Pixels, int32 Width, int32 Height)
{
	using namespace PythonImagePrivate;
	if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
	{
		return nullptr;
	}
	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
	{
		return nullptr;
	}
	FColor* Mip = static_cast<FColor*>(Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE)); //BGRA in memory
	for (int32 i = 0; i < Pixels.Num(); ++i)
	{
		const FLinearColor& P = Pixels[i];
		Mip[i] = FColor(ToByte(P.R), ToByte(P.G), ToByte(P.B), ToByte(P.A));
	}
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}

UTexture2D* UPythonImageLibrary::TensorToTexture(const FPyTensor& Tensor, bool bChannelsFirst)
{
	TArray<FLinearColor> Pixels;
	int32 W = 0, H = 0;
	FString Error;
	if (!TensorToPixels(Tensor, bChannelsFirst, Pixels, W, H, Error))
	{
		UE_LOG(LogPythonCLI, Warning, TEXT("TensorToTexture: %s"), *Error);
		return nullptr;
	}
	return MakeTexture(Pixels, W, H);
}

bool UPythonImageLibrary::UpdateTextureFromTensor(UTexture2D* Texture, const FPyTensor& Tensor, bool bChannelsFirst)
{
	using namespace PythonImagePrivate;
	TArray<FLinearColor> Pixels;
	int32 W = 0, H = 0;
	FString Error;
	if (!Texture || !TensorToPixels(Tensor, bChannelsFirst, Pixels, W, H, Error))
	{
		UE_LOG(LogPythonCLI, Warning, TEXT("UpdateTextureFromTensor: %s"), Texture ? *Error : TEXT("no texture"));
		return false;
	}
	if (Texture->GetSizeX() != W || Texture->GetSizeY() != H || Texture->GetPixelFormat() != PF_B8G8R8A8)
	{
		UE_LOG(LogPythonCLI, Warning, TEXT("UpdateTextureFromTensor: the texture is %dx%d (BGRA8 expected), the tensor %dx%d"), Texture->GetSizeX(), Texture->GetSizeY(), W, H);
		return false;
	}
	FColor* Data = new FColor[W * H];
	for (int32 i = 0; i < W * H; ++i)
	{
		const FLinearColor& P = Pixels[i];
		Data[i] = FColor(ToByte(P.R), ToByte(P.G), ToByte(P.B), ToByte(P.A));
	}
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, W, H);
	Texture->UpdateTextureRegions(0, 1, Region, W * sizeof(FColor), sizeof(FColor), reinterpret_cast<uint8*>(Data),
		[](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
		{
			delete[] reinterpret_cast<FColor*>(SrcData);
			delete Regions;
		});
	return true;
}

bool UPythonImageLibrary::WriteTensorToRenderTarget(const FPyTensor& Tensor, UTextureRenderTarget2D* RenderTarget, bool bChannelsFirst)
{
	using namespace PythonImagePrivate;
	TArray<FLinearColor> Pixels;
	int32 W = 0, H = 0;
	FString Error;
	if (!RenderTarget || !TensorToPixels(Tensor, bChannelsFirst, Pixels, W, H, Error))
	{
		UE_LOG(LogPythonCLI, Warning, TEXT("WriteTensorToRenderTarget: %s"), RenderTarget ? *Error : TEXT("no render target"));
		return false;
	}
	if (RenderTarget->SizeX != W || RenderTarget->SizeY != H)
	{
		RenderTarget->ResizeTarget(W, H);
	}
	const EPixelFormat Format = RenderTarget->GetFormat();
	TArray<uint8> Data;
	uint32 Pitch = 0;
	switch (Format)
	{
	case PF_B8G8R8A8:
	case PF_R8G8B8A8:
	{
		Pitch = W * 4;
		Data.SetNumUninitialized(Pitch * H);
		const bool bBGRA = Format == PF_B8G8R8A8;
		for (int32 i = 0; i < W * H; ++i)
		{
			const FLinearColor& P = Pixels[i];
			uint8* Out = Data.GetData() + i * 4;
			Out[0] = ToByte(bBGRA ? P.B : P.R);
			Out[1] = ToByte(P.G);
			Out[2] = ToByte(bBGRA ? P.R : P.B);
			Out[3] = ToByte(P.A);
		}
		break;
	}
	case PF_FloatRGBA:
	{
		Pitch = W * sizeof(FFloat16Color);
		Data.SetNumUninitialized(Pitch * H);
		FFloat16Color* Out = reinterpret_cast<FFloat16Color*>(Data.GetData());
		for (int32 i = 0; i < W * H; ++i)
		{
			Out[i] = FFloat16Color(Pixels[i]);
		}
		break;
	}
	case PF_A32B32G32R32F:
		Pitch = W * sizeof(FLinearColor);
		Data.SetNumUninitialized(Pitch * H);
		FMemory::Memcpy(Data.GetData(), Pixels.GetData(), Data.Num());
		break;
	default:
		UE_LOG(LogPythonCLI, Warning, TEXT("WriteTensorToRenderTarget: render target format %s isn't supported (RGBA8, RGBA16f or RGBA32f)"), GetPixelFormatString(Format));
		return false;
	}
	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		return false;
	}
	ENQUEUE_RENDER_COMMAND(PythonWriteRenderTarget)([Resource, Data = MoveTemp(Data), W, H, Pitch](FRHICommandListImmediate& RHICmdList)
	{
		FRHITexture* Texture = Resource->GetRenderTargetTexture();
		if (Texture && (int32)Texture->GetSizeX() == W && (int32)Texture->GetSizeY() == H)
		{
			RHICmdList.UpdateTexture2D(Texture, 0, FUpdateTextureRegion2D(0, 0, 0, 0, W, H), Pitch, Data.GetData());
		}
	});
	return true;
}
