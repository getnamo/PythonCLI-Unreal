#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "PythonTypes.h"
#include "PythonTestReceiver.generated.h"

/** Collects UPythonComponent events for the automation tests (game thread). */
UCLASS()
class UPythonTestReceiver : public UObject
{
	GENERATED_BODY()
public:
	TArray<FString> Started;
	TArray<FString> Stopped;
	TArray<FString> Errors;
	TArray<FString> Tracebacks;
	TArray<FPythonEventData> Events;
	TArray<FString> Logs;

	UFUNCTION()
	void OnStarted(const FString& Script) { Started.Add(Script); }

	UFUNCTION()
	void OnStopped(const FString& Script) { Stopped.Add(Script); }

	UFUNCTION()
	void OnError(const FString& Message, const FString& Traceback) { Errors.Add(Message); Tracebacks.Add(Traceback); }

	UFUNCTION()
	void OnEvent(const FString& EventName, const FString& JsonArgs, const FPythonEventData& Data) { Events.Add(Data); }

	UFUNCTION()
	void OnLog(const FString& Message, const FString& Level) { Logs.Add(Level + TEXT(": ") + Message); }

	const FPythonEventData* FindEvent(const FString& Name) const
	{
		return Events.FindByPredicate([&Name](const FPythonEventData& E) { return E.Name == Name; });
	}
};

/** Collects UPythonMLComponent events for the automation tests (game thread). */
UCLASS()
class UPythonMLTestReceiver : public UObject
{
	GENERATED_BODY()
public:
	int32 Setups = 0;
	int32 TrainingStarts = 0;
	TArray<TPair<FString, FString>> Results; //FunctionName, JsonResult
	TArray<TPair<FString, TArray<float>>> FloatResults;
	TArray<FString> Progress;
	TArray<TPair<FString, bool>> Completions; //JsonSummary, bStoppedEarly
	TArray<TPair<FString, FString>> CustomEvents; //EventName, Data
	TArray<FPythonCallResult> AsyncResults;

	UFUNCTION()
	void OnAsyncResult(const FPythonCallResult& Result) { AsyncResults.Add(Result); }

	UFUNCTION()
	void OnSetupComplete() { ++Setups; }

	UFUNCTION()
	void OnInputResults(const FString& JsonResult, const FString& FunctionName) { Results.Emplace(FunctionName, JsonResult); }

	UFUNCTION()
	void OnFloatArrayResults(const TArray<float>& Result, const FString& FunctionName) { FloatResults.Emplace(FunctionName, Result); }

	UFUNCTION()
	void OnTrainingStarted() { ++TrainingStarts; }

	UFUNCTION()
	void OnTrainingProgress(const FString& Json) { Progress.Add(Json); }

	UFUNCTION()
	void OnTrainingComplete(const FString& JsonSummary, bool bStoppedEarly) { Completions.Emplace(JsonSummary, bStoppedEarly); }

	UFUNCTION()
	void OnCustomEvent(const FString& EventName, const FString& Data) { CustomEvents.Emplace(EventName, Data); }

	const FString* FindResult(const FString& FunctionName) const
	{
		const TPair<FString, FString>* Found = Results.FindByPredicate([&FunctionName](const TPair<FString, FString>& R) { return R.Key == FunctionName; });
		return Found ? &Found->Value : nullptr;
	}
};
