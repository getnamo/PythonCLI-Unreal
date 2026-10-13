#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PythonTypes.h"
#include "PythonBlueprintLibrary.generated.h"

UCLASS()
class PYTHONCLI_API UPythonBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** Float32 tensor. Shape defaults to [Num] (row-major; e.g. [Height, Width, Channels]). */
	UFUNCTION(BlueprintPure, Category = "Python|Tensor", meta = (AutoCreateRefTerm = "Shape"))
	static FPyTensor MakeTensorFromFloats(const TArray<float>& Values, const TArray<int64>& Shape);

	/** UInt8 tensor (arrives in Python as a uint8 array). Shape defaults to [Num]. */
	UFUNCTION(BlueprintPure, Category = "Python|Tensor", meta = (AutoCreateRefTerm = "Shape"))
	static FPyTensor MakeTensorFromBytes(const TArray<uint8>& Bytes, const TArray<int64>& Shape);

	/** Element values as floats (converted from the tensor's dtype). */
	UFUNCTION(BlueprintPure, Category = "Python|Tensor")
	static TArray<float> TensorToFloats(const FPyTensor& Tensor);

	UFUNCTION(BlueprintPure, Category = "Python|Tensor")
	static int64 GetTensorNumElements(const FPyTensor& Tensor);

	/** Data size matches dtype x shape. */
	UFUNCTION(BlueprintPure, Category = "Python|Tensor")
	static bool IsTensorValid(const FPyTensor& Tensor);

	/** A number in JSON such as a call or step result. Path: "" for the value itself, else fields and array
	 indices separated by dots ("force", "action.move", "values.2"). */
	UFUNCTION(BlueprintPure, Category = "Python|JSON", meta = (AutoCreateRefTerm = "Path"))
	static bool GetJsonNumber(const FString& Json, const FString& Path, double& Value);

	/** A string, number or bool in JSON as text (path as in GetJsonNumber). */
	UFUNCTION(BlueprintPure, Category = "Python|JSON", meta = (AutoCreateRefTerm = "Path"))
	static bool GetJsonString(const FString& Json, const FString& Path, FString& Value);

	/** An array of numbers in JSON (path as in GetJsonNumber). */
	UFUNCTION(BlueprintPure, Category = "Python|JSON", meta = (AutoCreateRefTerm = "Path"))
	static bool GetJsonFloats(const FString& Json, const FString& Path, TArray<float>& Values);
};
