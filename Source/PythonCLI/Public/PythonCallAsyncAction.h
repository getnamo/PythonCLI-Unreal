#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "PythonTypes.h"
#include "PythonCallAsyncAction.generated.h"

class UPythonComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPythonCallResultSignature, const FPythonCallResult&, Result);

/** Blueprint: Call Python Function, with OnSuccess / OnFailure exec pins. */
UCLASS()
class PYTHONCLI_API UPythonCallAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FPythonCallResultSignature OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPythonCallResultSignature OnFailure;

	/** Call a function in the component's script (a module-level function or @ue.on handler) and get its
	 return value. JsonArgs: JSON array of arguments; Tensors are appended as numpy arrays. */
	UFUNCTION(BlueprintCallable, Category = "Python", meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "JsonArgs,Tensors", DisplayName = "Call Python Function"))
	static UPythonCallAsyncAction* CallPythonFunction(UPythonComponent* Component, const FString& FunctionName, const FString& JsonArgs, const TArray<FPyTensor>& Tensors);

	virtual void Activate() override;

private:
	TWeakObjectPtr<UPythonComponent> Component;
	FString FunctionName;
	FString JsonArgs;
	TArray<FPyTensor> Tensors;
};
