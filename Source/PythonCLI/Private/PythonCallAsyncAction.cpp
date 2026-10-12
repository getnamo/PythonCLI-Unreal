#include "PythonCallAsyncAction.h"
#include "PythonComponent.h"

UPythonCallAsyncAction* UPythonCallAsyncAction::CallPythonFunction(UPythonComponent* InComponent, const FString& InFunctionName, const FString& InJsonArgs, const TArray<FPyTensor>& InTensors)
{
	UPythonCallAsyncAction* Action = NewObject<UPythonCallAsyncAction>();
	Action->Component = InComponent;
	Action->FunctionName = InFunctionName;
	Action->JsonArgs = InJsonArgs;
	Action->Tensors = InTensors;
	if (InComponent)
	{
		Action->RegisterWithGameInstance(InComponent);
	}
	return Action;
}

void UPythonCallAsyncAction::Activate()
{
	UPythonComponent* Target = Component.Get();
	if (!Target)
	{
		FPythonCallResult R;
		R.Error = TEXT("Call Python Function: no Python component");
		OnFailure.Broadcast(R);
		SetReadyToDestroy();
		return;
	}
	TWeakObjectPtr<UPythonCallAsyncAction> Weak(this);
	Target->CallFunction(FunctionName, JsonArgs, Tensors, [Weak](const FPythonCallResult& Result)
	{
		if (UPythonCallAsyncAction* Self = Weak.Get())
		{
			(Result.bSuccess ? Self->OnSuccess : Self->OnFailure).Broadcast(Result);
			Self->SetReadyToDestroy();
		}
	});
}
