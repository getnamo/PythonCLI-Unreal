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
