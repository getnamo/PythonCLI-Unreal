#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "PythonTypes.generated.h"

UENUM(BlueprintType)
enum class EPyDType : uint8
{
	Float32,
	Float64,
	Float16,
	Int8,
	UInt8,
	Int16,
	UInt16,
	Int32,
	UInt32,
	Int64,
	UInt64,
	Bool
};

/** A tensor passed to/from Python as raw little-endian bytes (row-major). Arrives in Python as a numpy array
 (or unreal_bridge.Tensor without numpy). Raw bytes from Python arrive as UInt8 with Shape [N]. */
USTRUCT(BlueprintType)
struct PYTHONCLI_API FPyTensor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Tensor")
	EPyDType DType = EPyDType::Float32;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Tensor")
	TArray<int64> Shape;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Tensor")
	TArray<uint8> Data;

	static FPyTensor FromFloats(TConstArrayView<float> Values, TArray<int64> InShape = {});
	static FPyTensor FromBytes(TArray<uint8> Bytes, TArray<int64> InShape = {});

	int64 NumElements() const;
	/** Data size matches dtype x shape. */
	bool IsValid() const;
	/** Element values converted to float (any numeric dtype except Float16). */
	TArray<float> ToFloats() const;

	static int32 ItemSize(EPyDType Type);
	static const TCHAR* DTypeName(EPyDType Type);
	static bool ParseDType(const FString& Name, EPyDType& Out);
};

/** An event from a Python script. JsonArgs is the JSON array of arguments, with binary values as
 {"_bin": i} / {"_tensor": i, ...} placeholders indexing Tensors. */
USTRUCT(BlueprintType)
struct PYTHONCLI_API FPythonEventData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Python Event")
	FString Name;

	UPROPERTY(BlueprintReadOnly, Category = "Python Event")
	FString Script;

	UPROPERTY(BlueprintReadOnly, Category = "Python Event")
	FString JsonArgs;

	UPROPERTY(BlueprintReadOnly, Category = "Python Event")
	TArray<FPyTensor> Tensors;

	/** Parsed JsonArgs (C++). */
	TArray<TSharedPtr<FJsonValue>> Args;
};

/** Result of CallFunction. JsonResult is the returned value as JSON (binary values as placeholders into Tensors). */
USTRUCT(BlueprintType)
struct PYTHONCLI_API FPythonCallResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Python Call")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "Python Call")
	FString JsonResult;

	UPROPERTY(BlueprintReadOnly, Category = "Python Call")
	TArray<FPyTensor> Tensors;

	UPROPERTY(BlueprintReadOnly, Category = "Python Call")
	FString Error;

	UPROPERTY(BlueprintReadOnly, Category = "Python Call")
	FString Traceback;

	/** Parsed JsonResult (C++). */
	TSharedPtr<FJsonValue> Result;
};

/** Which script a component runs. */
USTRUCT(BlueprintType)
struct PYTHONCLI_API FPythonScriptParams
{
	GENERATED_BODY()

	/** Path of the .py file under ScriptRoot (e.g. "my_agent.py"). If it isn't found there, the
	 PythonCLI plugin's own Content/Python/ is tried (bundled examples, e.g. "examples/echo.py"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Script")
	FString Script;

	/** Relative to the project directory. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Script")
	FString ScriptRoot = TEXT("Content/Scripts/");

	/** Passed to the script as unreal_bridge.args. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Script")
	TArray<FString> Args;
};

/** Packing values for the wire: JSON args with binary placeholders + a binary table. */
namespace PythonWire
{
	/** Frame types shared with unreal_bridge/_frames.py. */
	enum EFrameType : uint8
	{
		Log = 1,
		RawLog = 2,
		Event = 3,
		Reply = 4,
		Control = 5,
		Status = 6,
	};

	/** Build an EVENT/CONTROL frame. */
	PYTHONCLI_API TArray<uint8> EncodeFrame(EFrameType Type, const TSharedRef<class FJsonObject>& Header, TConstArrayView<uint8> Binary = {});

	/** JsonArgs (a JSON array, or a single value wrapped into one) plus tensors appended as
	 {"_tensor": i, "dtype", "shape"} unless JsonArgs already references them by placeholder.
	 Returns false (with Error) if JsonArgs isn't valid JSON. */
	PYTHONCLI_API bool PackArgs(const FString& JsonArgs, const TArray<FPyTensor>& Tensors, TArray<TSharedPtr<FJsonValue>>& OutArgs, TArray<uint8>& OutBinaryTable, FString& Error);

	/** Binary table + the placeholders found in Value -> tensors (dtype/shape from {"_tensor"} descriptors,
	 UInt8 [N] for {"_bin"}). */
	PYTHONCLI_API TArray<FPyTensor> UnpackTensors(const TSharedPtr<FJsonValue>& Value, TConstArrayView<uint8> BinaryTable);

	PYTHONCLI_API FString ToJsonString(const TSharedPtr<FJsonValue>& Value);
	PYTHONCLI_API FString ToJsonString(const TArray<TSharedPtr<FJsonValue>>& Values);
}
