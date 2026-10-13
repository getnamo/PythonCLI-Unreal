#include "PythonObservationStreamComponent.h"
#include "PythonCLI.h"
#include "Dom/JsonObject.h"

UPythonObservationStreamComponent::UPythonObservationStreamComponent()
{
	StreamName = TEXT("observations");
	Policy = EPythonStreamPolicy::Queue;
	Slots = 4;
}

int64 UPythonObservationStreamComponent::GetFrameBytes() const
{
	return (int64)FMath::Max(RowsPerFrame, 1) * FMath::Max(Features.Num(), 1) * sizeof(float);
}

TSharedPtr<FJsonObject> UPythonObservationStreamComponent::MakeStreamInfo() const
{
	TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Names;
	for (const FString& Name : Features)
	{
		Names.Add(MakeShared<FJsonValueString>(Name));
	}
	Info->SetArrayField(TEXT("features"), Names);
	Info->SetNumberField(TEXT("rows_per_frame"), RowsPerFrame);
	return Info;
}

int32 UPythonObservationStreamComponent::FeatureIndex(const FString& Name)
{
	if (Index.Num() != Features.Num())
	{
		Index.Reset();
		for (int32 i = 0; i < Features.Num(); ++i)
		{
			Index.Add(Features[i], i);
		}
	}
	if (const int32* Found = Index.Find(Name))
	{
		return *Found;
	}
	if (!Warned.Contains(Name))
	{
		Warned.Add(Name);
		UE_LOG(LogPythonCLI, Warning, TEXT("Python stream '%s': '%s' is not one of its Features"), *StreamName, *Name);
	}
	return INDEX_NONE;
}

void UPythonObservationStreamComponent::SetFeature(const FString& Name, float Value)
{
	const int32 i = FeatureIndex(Name);
	if (i != INDEX_NONE)
	{
		Row.SetNumZeroed(Features.Num());
		Row[i] = Value;
	}
}

void UPythonObservationStreamComponent::SetFeatureVector(const FString& Name, FVector Value)
{
	SetFeature(Name + TEXT(".x"), (float)Value.X);
	SetFeature(Name + TEXT(".y"), (float)Value.Y);
	SetFeature(Name + TEXT(".z"), (float)Value.Z);
}

void UPythonObservationStreamComponent::SetFeatures(const TArray<float>& Values)
{
	Row = Values;
	Row.SetNumZeroed(Features.Num());
}

FPythonStreamFrame UPythonObservationStreamComponent::MakeFrame(int32 Rows) const
{
	FPythonStreamFrame Frame;
	Frame.DType = EPyDType::Float32;
	Frame.Shape = { Rows, Features.Num() };
	Frame.NumBytes = (int64)Rows * Features.Num() * sizeof(float);
	return Frame;
}

bool UPythonObservationStreamComponent::CommitRow()
{
	Row.SetNumZeroed(Features.Num());
	Batch.Append(Row);
	return Features.Num() > 0 && Batch.Num() >= FMath::Max(RowsPerFrame, 1) * Features.Num() ? Flush() : false;
}

bool UPythonObservationStreamComponent::Flush()
{
	if (Batch.Num() == 0 || Features.Num() == 0)
	{
		return false;
	}
	const int32 Rows = Batch.Num() / Features.Num();
	const bool bSent = Stream.IsValid() && Stream->Push(Batch.GetData(), MakeFrame(Rows));
	Batch.Reset();
	return bSent;
}

bool UPythonObservationStreamComponent::Step(FPythonCallResult& Result)
{
	Row.SetNumZeroed(Features.Num());
	Batch.Append(Row);
	const int32 Rows = Features.Num() > 0 ? Batch.Num() / Features.Num() : 0;
	if (!Stream.IsValid() || Rows == 0)
	{
		Batch.Reset();
		Result = FPythonCallResult();
		Result.Error = Stream.IsValid() ? TEXT("no Features") : TEXT("the stream is not open");
		return false;
	}
	//CommitRow sends full frames, so at most RowsPerFrame rows are here
	const bool bOk = Stream->Step(Batch.GetData(), MakeFrame(Rows), StepTimeoutSeconds, Result);
	Batch.Reset();
	return bOk;
}
