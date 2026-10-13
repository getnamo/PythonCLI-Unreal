#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "PythonCallAsyncAction.h"
#include "PythonTypes.h"
#include "PythonMLAsyncAction.generated.h"

class UPythonMLComponent;

/** Blueprint: ML inputs as async nodes, with the result (or error) on OnSuccess / OnFailure instead of the
 component's result events. */
UCLASS()
class PYTHONCLI_API UPythonMLAsyncAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable)
	FPythonCallResultSignature OnSuccess;

	UPROPERTY(BlueprintAssignable)
	FPythonCallResultSignature OnFailure;

	/** on_json_input(data) (or FunctionName). Json: any JSON value. */
	UFUNCTION(BlueprintCallable, Category = "Python ML", meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "FunctionName", DisplayName = "Send Json Input (Async)"))
	static UPythonMLAsyncAction* SendJsonInputAsync(UPythonMLComponent* Component, const FString& Json, const FString& FunctionName);

	/** on_tensor_input(array) (or FunctionName). Returned arrays are in Result.Tensors. */
	UFUNCTION(BlueprintCallable, Category = "Python ML", meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "FunctionName", DisplayName = "Send Tensor Input (Async)"))
	static UPythonMLAsyncAction* SendTensorInputAsync(UPythonMLComponent* Component, const FPyTensor& Tensor, const FString& FunctionName);

	/** A method of the script's API object. JsonArgs: a JSON array of arguments, or one value. */
	UFUNCTION(BlueprintCallable, Category = "Python ML", meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "JsonArgs", DisplayName = "Call Custom Function (Async)"))
	static UPythonMLAsyncAction* CallCustomFunctionAsync(UPythonMLComponent* Component, const FString& FunctionName, const FString& JsonArgs);

	virtual void Activate() override;

private:
	static UPythonMLAsyncAction* Make(UPythonMLComponent* Component, const FString& Op, const FString& Name, const FString& Json, const TArray<FPyTensor>& Tensors);

	TWeakObjectPtr<UPythonMLComponent> Component;
	FString Op;
	FString Name;
	FString Json;
	TArray<FPyTensor> Tensors;
};
