#include "PythonStreamComponent.h"
#include "PythonCLI.h"
#include "PythonComponent.h"
#include "PythonProcess.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

UPythonStreamComponent::UPythonStreamComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

UPythonComponent* UPythonStreamComponent::ResolvePythonComponent()
{
	if (!PythonComponent && GetOwner())
	{
		PythonComponent = GetOwner()->FindComponentByClass<UPythonComponent>();
	}
	return PythonComponent;
}

bool UPythonStreamComponent::OpenStream()
{
	if (Stream.IsValid() && Stream->IsOpen())
	{
		return true;
	}
	UPythonComponent* Python = ResolvePythonComponent();
	if (!Python)
	{
		UE_LOG(LogPythonCLI, Error, TEXT("Python stream '%s' (%s): no Python component to stream to"), *StreamName, *GetPathName());
		return false;
	}
	const TSharedPtr<FPythonProcess, ESPMode::ThreadSafe> Process = Python->GetProcess();
	if (!Process.IsValid())
	{
		UE_LOG(LogPythonCLI, Error, TEXT("Python stream '%s': Python is not available"), *StreamName);
		return false;
	}
	FPythonStreamConfig Config;
	Config.Owner = Python->GetOwnerId();
	Config.Script = Python->ScriptParams.Script;
	Config.Name = StreamName;
	Config.Policy = Policy;
	Config.Transport = Transport;
	Config.Slots = Slots;
	Config.SlotBytes = GetFrameBytes();
	Config.Info = MakeStreamInfo();
	FString Error;
	Stream = FPythonStream::Open(Process.ToSharedRef(), Config, Error);
	if (!Stream.IsValid())
	{
		UE_LOG(LogPythonCLI, Error, TEXT("Python stream '%s': %s"), *StreamName, *Error);
		return false;
	}
	Stream->MaxWaitSeconds = MaxWaitSeconds;
	OnStreamOpened();
	return true;
}

void UPythonStreamComponent::CloseStream()
{
	if (!Stream.IsValid())
	{
		return;
	}
	OnStreamClosing();
	Stream->Close();
	Stream.Reset();
}

bool UPythonStreamComponent::IsStreamOpen() const
{
	return Stream.IsValid() && Stream->IsOpen();
}

void UPythonStreamComponent::BeginPlay()
{
	Super::BeginPlay();
	if (bOpenOnBeginPlay)
	{
		OpenStream();
	}
}

void UPythonStreamComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	CloseStream();
	Super::EndPlay(EndPlayReason);
}

bool UPythonStreamComponent::PushTensor(const FPyTensor& Tensor, const FString& MetaJson)
{
	if (!Stream.IsValid() || !Tensor.IsValid())
	{
		return false;
	}
	FPythonStreamFrame Frame;
	Frame.DType = Tensor.DType;
	Frame.Shape = Tensor.Shape;
	Frame.NumBytes = Tensor.Data.Num();
	if (!MetaJson.IsEmpty())
	{
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(MetaJson), Frame.Meta);
	}
	return Stream->Push(Tensor.Data.GetData(), Frame);
}

bool UPythonStreamComponent::StepTensor(const FPyTensor& Tensor, FPythonCallResult& Result)
{
	if (!Stream.IsValid())
	{
		Result = FPythonCallResult();
		Result.Error = TEXT("the stream is not open");
		return false;
	}
	FPythonStreamFrame Frame;
	Frame.DType = Tensor.DType;
	Frame.Shape = Tensor.Shape;
	Frame.NumBytes = Tensor.Data.Num();
	return Stream->Step(Tensor.Data.GetData(), Frame, StepTimeoutSeconds, Result);
}

FPythonStreamStats UPythonStreamComponent::GetStreamStats() const
{
	return Stream.IsValid() ? Stream->GetStats() : FPythonStreamStats();
}
