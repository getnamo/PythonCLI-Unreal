#pragma once
// Shared by the PythonCLI automation tests.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "PythonComponent.h"
#include "PythonTestReceiver.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace PythonCLITests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter;
	constexpr double Timeout = 60.0; //First run creates the venv
	static const TCHAR* Fixture = TEXT("tests/scripts/fixture.py");

	struct FEnv
	{
		UGameInstance* GameInstance = nullptr;
		UWorld* World = nullptr;
		TArray<TWeakObjectPtr<AActor>> Actors;
		TArray<UPythonTestReceiver*> Receivers;

		void Create()
		{
			GameInstance = NewObject<UGameInstance>(GEngine);
			GameInstance->AddToRoot();
			GameInstance->InitializeStandalone();
			World = GameInstance->GetWorld();
			World->InitializeActorsForPlay(FURL());
		}

		/** Configure runs before registration/BeginPlay, like editing the component's details. */
		UPythonComponent* Add(UPythonTestReceiver*& OutReceiver, TFunctionRef<void(UPythonComponent*)> Configure)
		{
			OutReceiver = NewObject<UPythonTestReceiver>();
			OutReceiver->AddToRoot();
			Receivers.Add(OutReceiver);
			AActor* Actor = World->SpawnActor<AActor>();
			UPythonComponent* C = NewObject<UPythonComponent>(Actor);
			C->ScriptParams.Script = Fixture;
			Configure(C);
			C->OnScriptStarted.AddDynamic(OutReceiver, &UPythonTestReceiver::OnStarted);
			C->OnScriptStopped.AddDynamic(OutReceiver, &UPythonTestReceiver::OnStopped);
			C->OnScriptError.AddDynamic(OutReceiver, &UPythonTestReceiver::OnError);
			C->OnEvent.AddDynamic(OutReceiver, &UPythonTestReceiver::OnEvent);
			C->OnLog.AddDynamic(OutReceiver, &UPythonTestReceiver::OnLog);
			C->RegisterComponent();
			Actor->DispatchBeginPlay();
			Actors.Add(Actor);
			return C;
		}

		void Destroy()
		{
			for (const TWeakObjectPtr<AActor>& A : Actors)
			{
				if (A.IsValid())
				{
					A->Destroy();
				}
			}
			GameInstance->Shutdown();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GameInstance->RemoveFromRoot();
			for (UPythonTestReceiver* R : Receivers)
			{
				R->RemoveFromRoot();
			}
		}
	};

	static void Step(TFunction<void()> Fn)
	{
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Fn]() { Fn(); return true; }));
	}

	static void WaitUntil(FAutomationTestBase* Test, const FString& Label, TFunction<bool()> Condition, double InTimeout = Timeout)
	{
		const TSharedRef<double> Deadline = MakeShared<double>(0.0);
		ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Test, Label, Condition, Deadline, InTimeout]() -> bool
		{
			if (*Deadline == 0.0)
			{
				*Deadline = FPlatformTime::Seconds() + InTimeout;
			}
			if (Condition())
			{
				return true;
			}
			if (FPlatformTime::Seconds() > *Deadline)
			{
				Test->AddError(TEXT("Timed out waiting for ") + Label);
				return true;
			}
			return false;
		}));
	}

	/** Call and keep the result in Out; Done flips when it arrives. */
	struct FCall
	{
		bool bDone = false;
		FPythonCallResult Result;
	};

	static TSharedRef<FCall> Call(UPythonComponent* C, const FString& Function, const FString& JsonArgs, const TArray<FPyTensor>& Tensors = {})
	{
		TSharedRef<FCall> Out = MakeShared<FCall>();
		C->CallFunction(Function, JsonArgs, Tensors, [Out](const FPythonCallResult& R) { Out->Result = R; Out->bDone = true; });
		return Out;
	}
}
