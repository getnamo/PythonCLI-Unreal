#include "PythonBlueprintLibrary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace PythonBlueprintPrivate
{
	/** The value at a dotted path of fields / array indices; null if there is none. */
	static TSharedPtr<FJsonValue> Find(const FString& Json, const FString& Path)
	{
		//Wrapped in an array: UE's reader won't take a bare scalar (a call returning 5) at the root
		TArray<TSharedPtr<FJsonValue>> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + Json + TEXT("]")), Root) || Root.Num() != 1 || !Root[0].IsValid())
		{
			return nullptr;
		}
		TSharedPtr<FJsonValue> Value = Root[0];
		TArray<FString> Parts;
		Path.ParseIntoArray(Parts, TEXT("."));
		for (const FString& Part : Parts)
		{
			if (Value->Type == EJson::Object)
			{
				Value = Value->AsObject()->TryGetField(Part);
			}
			else if (Value->Type == EJson::Array && Part.IsNumeric())
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				const int32 Index = FCString::Atoi(*Part);
				Value = Items.IsValidIndex(Index) ? Items[Index] : nullptr;
			}
			else
			{
				return nullptr;
			}
			if (!Value.IsValid())
			{
				return nullptr;
			}
		}
		return Value;
	}
}

FPyTensor UPythonBlueprintLibrary::MakeTensorFromFloats(const TArray<float>& Values, const TArray<int64>& Shape)
{
	return FPyTensor::FromFloats(Values, Shape);
}

FPyTensor UPythonBlueprintLibrary::MakeTensorFromBytes(const TArray<uint8>& Bytes, const TArray<int64>& Shape)
{
	return FPyTensor::FromBytes(Bytes, Shape);
}

TArray<float> UPythonBlueprintLibrary::TensorToFloats(const FPyTensor& Tensor)
{
	return Tensor.ToFloats();
}

int64 UPythonBlueprintLibrary::GetTensorNumElements(const FPyTensor& Tensor)
{
	return Tensor.NumElements();
}

bool UPythonBlueprintLibrary::IsTensorValid(const FPyTensor& Tensor)
{
	return Tensor.IsValid();
}

bool UPythonBlueprintLibrary::GetJsonNumber(const FString& Json, const FString& Path, double& Value)
{
	const TSharedPtr<FJsonValue> Found = PythonBlueprintPrivate::Find(Json, Path);
	return Found.IsValid() && Found->TryGetNumber(Value);
}

bool UPythonBlueprintLibrary::GetJsonString(const FString& Json, const FString& Path, FString& Value)
{
	const TSharedPtr<FJsonValue> Found = PythonBlueprintPrivate::Find(Json, Path);
	return Found.IsValid() && Found->TryGetString(Value);
}

bool UPythonBlueprintLibrary::GetJsonFloats(const FString& Json, const FString& Path, TArray<float>& Values)
{
	Values.Reset();
	const TSharedPtr<FJsonValue> Found = PythonBlueprintPrivate::Find(Json, Path);
	if (!Found.IsValid() || Found->Type != EJson::Array)
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Item : Found->AsArray())
	{
		double Number = 0.0;
		if (!Item.IsValid() || !Item->TryGetNumber(Number))
		{
			Values.Reset();
			return false;
		}
		Values.Add((float)Number);
	}
	return true;
}
