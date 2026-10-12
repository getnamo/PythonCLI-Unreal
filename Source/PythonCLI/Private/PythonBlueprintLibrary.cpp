#include "PythonBlueprintLibrary.h"

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
