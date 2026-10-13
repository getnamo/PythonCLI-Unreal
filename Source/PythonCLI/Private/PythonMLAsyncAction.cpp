#include "PythonMLAsyncAction.h"
#include "PythonMLComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

UPythonMLAsyncAction* UPythonMLAsyncAction::Make(UPythonMLComponent* InComponent, const FString& InOp, const FString& InName, const FString& InJson, const TArray<FPyTensor>& InTensors)
{
	UPythonMLAsyncAction* Action = NewObject<UPythonMLAsyncAction>();
	Action->Component = InComponent;
	Action->Op = InOp;
	Action->Name = InName;
	Action->Json = InJson;
	Action->Tensors = InTensors;
	if (InComponent)
	{
		Action->RegisterWithGameInstance(InComponent);
	}
	return Action;
}

UPythonMLAsyncAction* UPythonMLAsyncAction::SendJsonInputAsync(UPythonMLComponent* InComponent, const FString& InJson, const FString& FunctionName)
{
	return Make(InComponent, TEXT("json_input"), FunctionName, InJson, {});
}

UPythonMLAsyncAction* UPythonMLAsyncAction::SendTensorInputAsync(UPythonMLComponent* InComponent, const FPyTensor& Tensor, const FString& FunctionName)
{
	return Make(InComponent, TEXT("tensor_input"), FunctionName, FString(), { Tensor });
}

UPythonMLAsyncAction* UPythonMLAsyncAction::CallCustomFunctionAsync(UPythonMLComponent* InComponent, const FString& FunctionName, const FString& JsonArgs)
{
	return Make(InComponent, TEXT("custom"), FunctionName, JsonArgs, {});
}

void UPythonMLAsyncAction::Activate()
{
	auto Fail = [this](const FString& Error)
	{
		FPythonCallResult R;
		R.Error = Error;
		OnFailure.Broadcast(R);
		SetReadyToDestroy();
	};
	UPythonMLComponent* Target = Component.Get();
	if (!Target)
	{
		Fail(TEXT("no Python ML component"));
		return;
	}
	TSharedPtr<FJsonValue> Input;
	if (!Json.TrimStartAndEnd().IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + Json + TEXT("]")), Root) || Root.Num() != 1)
		{
			Fail(FString::Printf(TEXT("not valid JSON: %s"), *Json.Left(200)));
			return;
		}
		Input = Root[0];
	}
	TWeakObjectPtr<UPythonMLAsyncAction> Weak(this);
	Target->CallML(Op, Name, Input, Tensors, [Weak](const FPythonCallResult& Result)
	{
		if (UPythonMLAsyncAction* Self = Weak.Get())
		{
			(Result.bSuccess ? Self->OnSuccess : Self->OnFailure).Broadcast(Result);
			Self->SetReadyToDestroy();
		}
	});
}
