#include "PythonTypes.h"
#include "CLIFrameCodec.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Math/Float16.h"

namespace PythonWire
{
	static const FCLIFrameMagic Magic('P', 'U', 'E', 1);
}

// ---------------------------------------------------------------------------------------------- FPyTensor

int32 FPyTensor::ItemSize(EPyDType Type)
{
	switch (Type)
	{
	case EPyDType::Float64: case EPyDType::Int64: case EPyDType::UInt64: return 8;
	case EPyDType::Float32: case EPyDType::Int32: case EPyDType::UInt32: return 4;
	case EPyDType::Float16: case EPyDType::Int16: case EPyDType::UInt16: return 2;
	default: return 1;
	}
}

const TCHAR* FPyTensor::DTypeName(EPyDType Type)
{
	switch (Type)
	{
	case EPyDType::Float32: return TEXT("float32");
	case EPyDType::Float64: return TEXT("float64");
	case EPyDType::Float16: return TEXT("float16");
	case EPyDType::Int8: return TEXT("int8");
	case EPyDType::UInt8: return TEXT("uint8");
	case EPyDType::Int16: return TEXT("int16");
	case EPyDType::UInt16: return TEXT("uint16");
	case EPyDType::Int32: return TEXT("int32");
	case EPyDType::UInt32: return TEXT("uint32");
	case EPyDType::Int64: return TEXT("int64");
	case EPyDType::UInt64: return TEXT("uint64");
	case EPyDType::Bool: return TEXT("bool");
	}
	return TEXT("uint8");
}

bool FPyTensor::ParseDType(const FString& Name, EPyDType& Out)
{
	for (uint8 i = 0; i <= (uint8)EPyDType::Bool; ++i)
	{
		if (Name == DTypeName((EPyDType)i))
		{
			Out = (EPyDType)i;
			return true;
		}
	}
	return false;
}

FPyTensor FPyTensor::FromFloats(TConstArrayView<float> Values, TArray<int64> InShape)
{
	FPyTensor T;
	T.DType = EPyDType::Float32;
	T.Shape = InShape.Num() > 0 ? MoveTemp(InShape) : TArray<int64>{ Values.Num() };
	T.Data.Append(reinterpret_cast<const uint8*>(Values.GetData()), Values.Num() * sizeof(float));
	return T;
}

FPyTensor FPyTensor::FromBytes(TArray<uint8> Bytes, TArray<int64> InShape)
{
	FPyTensor T;
	T.DType = EPyDType::UInt8;
	T.Shape = InShape.Num() > 0 ? MoveTemp(InShape) : TArray<int64>{ Bytes.Num() };
	T.Data = MoveTemp(Bytes);
	return T;
}

int64 FPyTensor::NumElements() const
{
	int64 N = 1;
	for (int64 D : Shape)
	{
		N *= D;
	}
	return N;
}

bool FPyTensor::IsValid() const
{
	return NumElements() * ItemSize(DType) == Data.Num();
}

TArray<float> FPyTensor::ToFloats() const
{
	TArray<float> Out;
	const int64 N = Data.Num() / ItemSize(DType);
	Out.SetNumUninitialized(N);
	const uint8* P = Data.GetData();
	for (int64 i = 0; i < N; ++i)
	{
		switch (DType)
		{
		case EPyDType::Float32: { float V; FMemory::Memcpy(&V, P + i * 4, 4); Out[i] = V; break; }
		case EPyDType::Float64: { double V; FMemory::Memcpy(&V, P + i * 8, 8); Out[i] = (float)V; break; }
		case EPyDType::Int8: Out[i] = (float)(int8)P[i]; break;
		case EPyDType::UInt8: case EPyDType::Bool: Out[i] = (float)P[i]; break;
		case EPyDType::Int16: { int16 V; FMemory::Memcpy(&V, P + i * 2, 2); Out[i] = V; break; }
		case EPyDType::UInt16: { uint16 V; FMemory::Memcpy(&V, P + i * 2, 2); Out[i] = V; break; }
		case EPyDType::Int32: { int32 V; FMemory::Memcpy(&V, P + i * 4, 4); Out[i] = (float)V; break; }
		case EPyDType::UInt32: { uint32 V; FMemory::Memcpy(&V, P + i * 4, 4); Out[i] = (float)V; break; }
		case EPyDType::Int64: { int64 V; FMemory::Memcpy(&V, P + i * 8, 8); Out[i] = (float)V; break; }
		case EPyDType::UInt64: { uint64 V; FMemory::Memcpy(&V, P + i * 8, 8); Out[i] = (float)V; break; }
		case EPyDType::Float16: { FFloat16 V; FMemory::Memcpy(&V, P + i * 2, 2); Out[i] = V.GetFloat(); break; }
		}
	}
	return Out;
}

// ---------------------------------------------------------------------------------------------- wire

namespace PythonWire
{
	TArray<uint8> EncodeFrame(EFrameType Type, const TSharedRef<FJsonObject>& Header, TConstArrayView<uint8> Binary)
	{
		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Header, Writer);
		FTCHARToUTF8 Utf8(*Json);
		TArray<uint8> Out;
		FCLIFrameCodec::EncodeTo(Out, Magic, (uint8)Type, TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), Binary);
		return Out;
	}

	FString ToJsonString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}
		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return Json;
	}

	FString ToJsonString(const TArray<TSharedPtr<FJsonValue>>& Values)
	{
		FString Json;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Values, Writer);
		return Json;
	}

	bool PackArgs(const FString& JsonArgs, const TArray<FPyTensor>& Tensors, TArray<TSharedPtr<FJsonValue>>& OutArgs, TArray<uint8>& OutBinaryTable, FString& Error)
	{
		OutArgs.Reset();
		const FString Trimmed = JsonArgs.TrimStartAndEnd();
		if (!Trimmed.IsEmpty())
		{
			// Parse as an array if it is one; otherwise wrap the single value.
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + Trimmed + TEXT("]")), OutArgs))
			{
				Error = FString::Printf(TEXT("arguments are not valid JSON: %s"), *JsonArgs.Left(200));
				return false;
			}
			if (Trimmed.StartsWith(TEXT("[")) && OutArgs.Num() == 1 && OutArgs[0]->Type == EJson::Array)
			{
				//Copy first: AsArray() refers into OutArgs[0], which assigning to OutArgs would free
				TArray<TSharedPtr<FJsonValue>> Inner = OutArgs[0]->AsArray();
				OutArgs = MoveTemp(Inner);
			}
		}

		if (Tensors.Num() > 0)
		{
			TArray<TArray<uint8>> Buffers;
			for (const FPyTensor& T : Tensors)
			{
				Buffers.Add(T.Data);
			}
			FCLIFrameCodec::BuildBinaryTable(Buffers, OutBinaryTable);
			const bool bReferenced = JsonArgs.Contains(TEXT("\"_tensor\"")) || JsonArgs.Contains(TEXT("\"_bin\""));
			if (!bReferenced)
			{
				for (int32 i = 0; i < Tensors.Num(); ++i)
				{
					TSharedRef<FJsonObject> Desc = MakeShared<FJsonObject>();
					Desc->SetNumberField(TEXT("_tensor"), i);
					Desc->SetStringField(TEXT("dtype"), FPyTensor::DTypeName(Tensors[i].DType));
					TArray<TSharedPtr<FJsonValue>> Shape;
					const TArray<int64> EffectiveShape = Tensors[i].Shape.Num() > 0 ? Tensors[i].Shape : TArray<int64>{ (int64)(Tensors[i].Data.Num() / FPyTensor::ItemSize(Tensors[i].DType)) };
					for (int64 D : EffectiveShape)
					{
						Shape.Add(MakeShared<FJsonValueNumber>((double)D));
					}
					Desc->SetArrayField(TEXT("shape"), Shape);
					OutArgs.Add(MakeShared<FJsonValueObject>(Desc));
				}
			}
		}
		return true;
	}

	static void CollectDescriptors(const TSharedPtr<FJsonValue>& Value, TMap<int32, TSharedPtr<FJsonObject>>& Out)
	{
		if (!Value.IsValid())
		{
			return;
		}
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& V : Value->AsArray())
			{
				CollectDescriptors(V, Out);
			}
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			double Index = 0;
			if (Obj->TryGetNumberField(TEXT("_tensor"), Index) || Obj->TryGetNumberField(TEXT("_bin"), Index))
			{
				Out.Add((int32)Index, Obj);
				return;
			}
			for (const auto& Pair : Obj->Values)
			{
				CollectDescriptors(Pair.Value, Out);
			}
		}
	}

	TArray<FPyTensor> UnpackTensors(const TSharedPtr<FJsonValue>& Value, TConstArrayView<uint8> BinaryTable)
	{
		TArray<FPyTensor> Out;
		TArray<TArray<uint8>> Buffers;
		if (BinaryTable.Num() == 0 || !FCLIFrameCodec::ParseBinaryTable(BinaryTable, Buffers))
		{
			return Out;
		}
		TMap<int32, TSharedPtr<FJsonObject>> Descriptors;
		CollectDescriptors(Value, Descriptors);
		Out.SetNum(Buffers.Num());
		for (int32 i = 0; i < Buffers.Num(); ++i)
		{
			FPyTensor& T = Out[i];
			T.Data = MoveTemp(Buffers[i]);
			T.DType = EPyDType::UInt8;
			T.Shape = { T.Data.Num() };
			const TSharedPtr<FJsonObject>* Desc = Descriptors.Find(i);
			FString DTypeName;
			if (Desc && (*Desc)->HasField(TEXT("_tensor")) && (*Desc)->TryGetStringField(TEXT("dtype"), DTypeName))
			{
				FPyTensor::ParseDType(DTypeName, T.DType);
				const TArray<TSharedPtr<FJsonValue>>* Shape = nullptr;
				if ((*Desc)->TryGetArrayField(TEXT("shape"), Shape))
				{
					T.Shape.Reset();
					for (const TSharedPtr<FJsonValue>& D : *Shape)
					{
						T.Shape.Add((int64)D->AsNumber());
					}
				}
			}
		}
		return Out;
	}
}
