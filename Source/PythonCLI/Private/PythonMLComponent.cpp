#include "PythonMLComponent.h"
#include "PythonCLI.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace PythonMLPrivate
{
	/** {"_tensor": i, "dtype", "shape"}: places a tensor among the arguments (else it's appended last). */
	static TSharedPtr<FJsonValue> TensorPlaceholder(const FPyTensor& T, int32 Index)
	{
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetNumberField(TEXT("_tensor"), Index);
		D->SetStringField(TEXT("dtype"), FPyTensor::DTypeName(T.DType));
		TArray<TSharedPtr<FJsonValue>> Shape;
		if (T.Shape.Num() > 0)
		{
			for (const int64 Dim : T.Shape)
			{
				Shape.Add(MakeShared<FJsonValueNumber>((double)Dim));
			}
		}
		else
		{
			Shape.Add(MakeShared<FJsonValueNumber>((double)(T.Data.Num() / FPyTensor::ItemSize(T.DType))));
		}
		D->SetArrayField(TEXT("shape"), Shape);
		return MakeShared<FJsonValueObject>(D);
	}

	/** Any JSON value (UE's reader won't take a bare scalar at the root, so it's wrapped). */
	static TSharedPtr<FJsonValue> ParseValue(const FString& Json)
	{
		if (Json.TrimStartAndEnd().IsEmpty())
		{
			return MakeShared<FJsonValueNull>();
		}
		TArray<TSharedPtr<FJsonValue>> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + Json + TEXT("]")), Root) || Root.Num() != 1)
		{
			return nullptr;
		}
		return Root[0];
	}

	static TSharedPtr<FJsonValue> NameOrNull(const FString& Name)
	{
		return Name.IsEmpty() ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()) : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Name));
	}

	static FString AsText(const TSharedPtr<FJsonValue>& Value)
	{
		FString Text;
		if (Value.IsValid() && Value->TryGetString(Text))
		{
			return Text;
		}
		return PythonWire::ToJsonString(Value);
	}
}

void UPythonMLComponent::BeginPlay()
{
	bTrainPending = bTrainOnBeginPlay;
	Super::BeginPlay();
}

void UPythonMLComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bStopTrainingOnEndPlay && bIsTraining)
	{
		CallReserved(TEXT("_ml_stop_training"), {}, {}, [](const FPythonCallResult&) {}); //Ahead of the script stop
	}
	bIsTraining = false;
	bIsSetUp = false;
	Super::EndPlay(EndPlayReason);
}

void UPythonMLComponent::CallReserved(const FString& Function, const TArray<TSharedPtr<FJsonValue>>& Args, const TArray<FPyTensor>& Tensors, TFunction<void(const FPythonCallResult&)> OnResult)
{
	CallFunction(Function, PythonWire::ToJsonString(Args), Tensors, MoveTemp(OnResult));
}

void UPythonMLComponent::CallML(const FString& Op, const FString& Name, const TSharedPtr<FJsonValue>& Input, const TArray<FPyTensor>& Tensors, TFunction<void(const FPythonCallResult&)> OnResult)
{
	using namespace PythonMLPrivate;
	const TSharedPtr<FJsonValue> Value = Input.IsValid() ? Input : MakeShared<FJsonValueNull>();
	if (Op == TEXT("json_input") || Op == TEXT("string_input"))
	{
		CallReserved(TEXT("_ml_json_input"), { Value, NameOrNull(Name) }, {}, MoveTemp(OnResult));
	}
	else if ((Op == TEXT("float_input") || Op == TEXT("tensor_input")) && Tensors.Num() > 0)
	{
		CallReserved(Op == TEXT("float_input") ? TEXT("_ml_float_input") : TEXT("_ml_tensor_input"), { TensorPlaceholder(Tensors[0], 0), NameOrNull(Name) }, { Tensors[0] }, MoveTemp(OnResult));
	}
	else if (Op == TEXT("custom"))
	{
		TArray<TSharedPtr<FJsonValue>> Args = { MakeShared<FJsonValueString>(Name) };
		if (Value->Type == EJson::Array)
		{
			Args.Append(Value->AsArray());
		}
		else if (Value->Type != EJson::Null)
		{
			Args.Add(Value);
		}
		CallReserved(TEXT("_ml_custom"), Args, Tensors, MoveTemp(OnResult));
	}
	else if (Op == TEXT("save_model") || Op == TEXT("load_model"))
	{
		CallReserved(TEXT("_ml_") + Op, { MakeShared<FJsonValueString>(Name) }, {}, MoveTemp(OnResult));
	}
	else if (OnResult)
	{
		FPythonCallResult R;
		R.Error = FString::Printf(TEXT("unknown ML call '%s'"), *Op);
		OnResult(R);
	}
}

TFunction<void(const FPythonCallResult&)> UPythonMLComponent::ResultHandler(const FString& FunctionName, bool bFloats)
{
	TWeakObjectPtr<UPythonMLComponent> Weak(this);
	return [Weak, FunctionName, bFloats](const FPythonCallResult& R)
	{
		UPythonMLComponent* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		if (!R.bSuccess)
		{
			Self->OnScriptError.Broadcast(R.Error, R.Traceback);
		}
		else if (bFloats)
		{
			Self->OnFloatArrayResults.Broadcast(ResultToFloats(R), FunctionName);
		}
		else
		{
			Self->OnInputResults.Broadcast(R.JsonResult, FunctionName);
		}
	};
}

TArray<float> UPythonMLComponent::ResultToFloats(const FPythonCallResult& Result)
{
	if (Result.Tensors.Num() > 0)
	{
		return Result.Tensors[0].ToFloats();
	}
	TArray<float> Out;
	if (Result.Result.IsValid() && Result.Result->Type == EJson::Array)
	{
		for (const TSharedPtr<FJsonValue>& V : Result.Result->AsArray())
		{
			double Number = 0.0;
			if (V.IsValid() && V->TryGetNumber(Number))
			{
				Out.Add((float)Number);
			}
		}
	}
	return Out;
}

void UPythonMLComponent::SendJsonInput(const FString& Json, const FString& FunctionName)
{
	const TSharedPtr<FJsonValue> Value = PythonMLPrivate::ParseValue(Json);
	if (!Value.IsValid())
	{
		const FString Error = FString::Printf(TEXT("SendJsonInput: not valid JSON: %s"), *Json.Left(200));
		UE_LOG(LogPythonCLI, Error, TEXT("%s"), *Error);
		OnScriptError.Broadcast(Error, FString());
		return;
	}
	CallML(TEXT("json_input"), FunctionName, Value, {}, ResultHandler(FunctionName.IsEmpty() ? TEXT("on_json_input") : FunctionName, false));
}

void UPythonMLComponent::SendStringInput(const FString& Text, const FString& FunctionName)
{
	CallML(TEXT("string_input"), FunctionName, MakeShared<FJsonValueString>(Text), {}, ResultHandler(FunctionName.IsEmpty() ? TEXT("on_json_input") : FunctionName, false));
}

void UPythonMLComponent::SendFloatArrayInput(const TArray<float>& Values, const FString& FunctionName)
{
	CallML(TEXT("float_input"), FunctionName, nullptr, { FPyTensor::FromFloats(Values) }, ResultHandler(FunctionName.IsEmpty() ? TEXT("on_float_array_input") : FunctionName, true));
}

void UPythonMLComponent::SendTensorInput(const FPyTensor& Tensor, const FString& FunctionName)
{
	if (!Tensor.IsValid())
	{
		OnScriptError.Broadcast(TEXT("SendTensorInput: the tensor's data doesn't match its dtype and shape"), FString());
		return;
	}
	CallML(TEXT("tensor_input"), FunctionName, nullptr, { Tensor }, ResultHandler(FunctionName.IsEmpty() ? TEXT("on_tensor_input") : FunctionName, false));
}

void UPythonMLComponent::CallCustomFunction(const FString& FunctionName, const FString& JsonArgs)
{
	const TSharedPtr<FJsonValue> Args = PythonMLPrivate::ParseValue(JsonArgs);
	if (!Args.IsValid())
	{
		OnScriptError.Broadcast(FString::Printf(TEXT("CallCustomFunction %s: arguments are not valid JSON"), *FunctionName), FString());
		return;
	}
	CallML(TEXT("custom"), FunctionName, Args, {}, ResultHandler(FunctionName, false));
}

void UPythonMLComponent::BeginTraining()
{
	TWeakObjectPtr<UPythonMLComponent> Weak(this);
	CallReserved(TEXT("_ml_begin_training"), {}, {}, [Weak](const FPythonCallResult& R)
	{
		UPythonMLComponent* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		if (!R.bSuccess)
		{
			Self->OnScriptError.Broadcast(R.Error, R.Traceback);
			return;
		}
		//Started/complete arrive as events from the training thread; this only reports a refusal
		const TSharedPtr<FJsonObject> Obj = R.Result.IsValid() && R.Result->Type == EJson::Object ? R.Result->AsObject() : nullptr;
		if (Obj.IsValid() && !Obj->GetBoolField(TEXT("started")))
		{
			UE_LOG(LogPythonCLI, Warning, TEXT("[python %s] BeginTraining: %s"), *Self->ScriptParams.Script, *Obj->GetStringField(TEXT("reason")));
		}
	});
}

void UPythonMLComponent::StopTraining()
{
	TWeakObjectPtr<UPythonMLComponent> Weak(this);
	CallReserved(TEXT("_ml_stop_training"), {}, {}, [Weak](const FPythonCallResult& R)
	{
		if (UPythonMLComponent* Self = Weak.Get(); Self && !R.bSuccess)
		{
			Self->OnScriptError.Broadcast(R.Error, R.Traceback);
		}
	});
}

void UPythonMLComponent::SaveModel(const FString& ModelName)
{
	CallML(TEXT("save_model"), ModelName, nullptr, {}, ResultHandler(TEXT("on_save_model"), false));
}

void UPythonMLComponent::LoadModel(const FString& ModelName)
{
	CallML(TEXT("load_model"), ModelName, nullptr, {}, ResultHandler(TEXT("on_load_model"), false));
}

void UPythonMLComponent::HandleScriptStarted(const FString& Script)
{
	bIsSetUp = false;
	bIsTraining = false;
	TWeakObjectPtr<UPythonMLComponent> Weak(this);
	CallReserved(TEXT("_ml_setup"), {}, {}, [Weak](const FPythonCallResult& R)
	{
		UPythonMLComponent* Self = Weak.Get();
		if (!Self)
		{
			return;
		}
		if (!R.bSuccess)
		{
			UE_LOG(LogPythonCLI, Error, TEXT("[python %s] ML setup failed: %s"), *Self->ScriptParams.Script, *R.Error);
			Self->OnScriptError.Broadcast(R.Error, R.Traceback);
			return;
		}
		Self->bIsSetUp = true;
		Self->OnSetupComplete.Broadcast();
		if (Self->bTrainPending)
		{
			Self->bTrainPending = false;
			Self->BeginTraining();
		}
	});
}

void UPythonMLComponent::HandleScriptStopped(const FString& Script)
{
	bIsSetUp = false;
	bIsTraining = false;
}

bool UPythonMLComponent::HandleScriptEvent(const FPythonEventData& Data)
{
	using namespace PythonMLPrivate;
	if (!Data.Name.StartsWith(TEXT("_ml_")))
	{
		return false;
	}
	auto Arg = [&Data](int32 Index) -> TSharedPtr<FJsonValue>
	{
		return Data.Args.IsValidIndex(Index) && Data.Args[Index].IsValid() ? Data.Args[Index] : MakeShared<FJsonValueNull>();
	};
	if (Data.Name == TEXT("_ml_progress"))
	{
		OnTrainingProgress.Broadcast(PythonWire::ToJsonString(Arg(0)));
	}
	else if (Data.Name == TEXT("_ml_training_started"))
	{
		bIsTraining = true;
		OnTrainingStarted.Broadcast();
	}
	else if (Data.Name == TEXT("_ml_training_complete"))
	{
		bIsTraining = false;
		bool bStopped = false;
		Arg(1)->TryGetBool(bStopped);
		OnTrainingComplete.Broadcast(PythonWire::ToJsonString(Arg(0)), bStopped);
	}
	else if (Data.Name == TEXT("_ml_training_error"))
	{
		bIsTraining = false;
		OnScriptError.Broadcast(AsText(Arg(0)), AsText(Arg(1)));
	}
	else if (Data.Name == TEXT("_ml_event"))
	{
		OnCustomEvent.Broadcast(AsText(Arg(0)), AsText(Arg(1)));
	}
	return true;
}
