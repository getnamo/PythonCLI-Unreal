// ML component tests (PythonCLI.ML.*): UPythonMLComponent driving unreal_bridge.ml scripts, no ML framework
// needed. Python side: tests/scripts/ml_fixture.py (snake_case) and ml_legacy.py (TFPluginAPI camelCase).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "PythonBlueprintLibrary.h"
#include "PythonMLAsyncAction.h"
#include "PythonMLComponent.h"
#include "PythonTestHelpers.h"
#include "PythonTestReceiver.h"

using namespace PythonCLITests;

namespace PythonMLTests
{
	struct FRig
	{
		TSharedRef<FEnv> Env = MakeShared<FEnv>();
		UPythonMLComponent* ML = nullptr;
		UPythonTestReceiver* R = nullptr;
		UPythonMLTestReceiver* M = nullptr;
	};

	static TSharedRef<FRig> MakeRig(const FString& Script, TFunction<void(UPythonMLComponent*)> Configure = nullptr)
	{
		TSharedRef<FRig> Rig = MakeShared<FRig>();
		FEnv& E = *Rig->Env;
		E.Create();
		Rig->R = NewObject<UPythonTestReceiver>();
		Rig->R->AddToRoot();
		E.Receivers.Add(Rig->R);
		Rig->M = NewObject<UPythonMLTestReceiver>();
		Rig->M->AddToRoot();

		AActor* Actor = E.World->SpawnActor<AActor>();
		UPythonMLComponent* C = NewObject<UPythonMLComponent>(Actor);
		C->ScriptParams.Script = Script;
		if (Configure)
		{
			Configure(C);
		}
		C->OnScriptStarted.AddDynamic(Rig->R, &UPythonTestReceiver::OnStarted);
		C->OnScriptStopped.AddDynamic(Rig->R, &UPythonTestReceiver::OnStopped);
		C->OnScriptError.AddDynamic(Rig->R, &UPythonTestReceiver::OnError);
		C->OnEvent.AddDynamic(Rig->R, &UPythonTestReceiver::OnEvent);
		C->OnSetupComplete.AddDynamic(Rig->M, &UPythonMLTestReceiver::OnSetupComplete);
		C->OnInputResults.AddDynamic(Rig->M, &UPythonMLTestReceiver::OnInputResults);
		C->OnFloatArrayResults.AddDynamic(Rig->M, &UPythonMLTestReceiver::OnFloatArrayResults);
		C->OnTrainingStarted.AddDynamic(Rig->M, &UPythonMLTestReceiver::OnTrainingStarted);
		C->OnTrainingProgress.AddDynamic(Rig->M, &UPythonMLTestReceiver::OnTrainingProgress);
		C->OnTrainingComplete.AddDynamic(Rig->M, &UPythonMLTestReceiver::OnTrainingComplete);
		C->OnCustomEvent.AddDynamic(Rig->M, &UPythonMLTestReceiver::OnCustomEvent);
		C->RegisterComponent();
		Actor->DispatchBeginPlay();
		E.Actors.Add(Actor);
		Rig->ML = C;
		return Rig;
	}

	static void Destroy(const TSharedRef<FRig>& Rig)
	{
		Rig->M->RemoveFromRoot();
		Rig->Env->Destroy();
	}

	static double Number(const FString& Json, const FString& Path)
	{
		double Value = -1.0;
		UPythonBlueprintLibrary::GetJsonNumber(Json, Path, Value);
		return Value;
	}
}

using namespace PythonMLTests;

// Inputs of every kind, custom functions and the async node; on_setup runs once, first.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonMLInputsTest, "PythonCLI.ML.Inputs", PythonCLITests::Flags)
bool FPythonMLInputsTest::RunTest(const FString&)
{
	TSharedRef<FRig> Rig = MakeRig(TEXT("tests/scripts/ml_fixture.py"));
	UPythonMLComponent* C = Rig->ML;
	//Sent before the script is even up: queued behind on_start, and on_setup runs before the first input
	C->SendJsonInput(TEXT("{\"a\": 2, \"b\": 3}"), FString());
	C->SendFloatArrayInput({ 1.f, 2.f, 3.f }, FString());
	FPyTensor T = FPyTensor::FromFloats({ 1.f, 2.f, 3.f, 6.f }, { 2, 2 });
	C->SendTensorInput(T, FString());
	C->CallCustomFunction(TEXT("scale"), TEXT("[4, 3]"));
	UPythonMLAsyncAction* Action = UPythonMLAsyncAction::SendJsonInputAsync(C, TEXT("{\"a\": 10, \"b\": 5}"), FString());
	Action->AddToRoot();
	Action->OnSuccess.AddDynamic(Rig->M, &UPythonMLTestReceiver::OnAsyncResult);
	Action->OnFailure.AddDynamic(Rig->M, &UPythonMLTestReceiver::OnAsyncResult);
	Action->Activate(); //What the Blueprint node does

	WaitUntil(this, TEXT("results"), [Rig] { return Rig->M->Results.Num() >= 3 && Rig->M->FloatResults.Num() >= 1 && Rig->M->AsyncResults.Num() == 1; });
	Step([this, Rig, Action]
	{
		UPythonMLTestReceiver* M = Rig->M;
		TestEqual(TEXT("setup ran once"), M->Setups, 1);
		const FString* Json = M->FindResult(TEXT("on_json_input"));
		TestTrue(TEXT("json input result"), Json && Number(*Json, TEXT("sum")) == 5.0 && Number(*Json, TEXT("setups")) == 1.0);
		TestTrue(TEXT("float array result"), M->FloatResults.Num() == 1 && M->FloatResults[0].Key == TEXT("on_float_array_input") && M->FloatResults[0].Value == TArray<float>({ 2.f, 4.f, 6.f }));
		const FString* Tensor = M->FindResult(TEXT("on_tensor_input"));
		TestTrue(TEXT("tensor input result"), Tensor && Number(*Tensor, TEXT("mean")) == 3.0 && Number(*Tensor, TEXT("shape.1")) == 2.0);
		const FString* Scale = M->FindResult(TEXT("scale"));
		TestTrue(TEXT("custom function result"), Scale && *Scale == TEXT("12"));
		TestTrue(TEXT("async node returns its result"), M->AsyncResults[0].bSuccess && Number(M->AsyncResults[0].JsonResult, TEXT("sum")) == 15.0);
		TestEqual(TEXT("async node results don't go to OnInputResults"), M->Results.Num(), 3);
		TestTrue(TEXT("component is set up"), Rig->ML->bIsSetUp);
		Action->RemoveFromRoot();
	});

	//The C++ form: the full result
	TSharedRef<FPythonCallResult> Full = MakeShared<FPythonCallResult>();
	TSharedRef<bool> bDone = MakeShared<bool>(false);
	Step([Rig, Full, bDone]
	{
		Rig->ML->CallML(TEXT("tensor_input"), FString(), nullptr, { FPyTensor::FromFloats({ 4.f, 8.f }) }, [Full, bDone](const FPythonCallResult& R) { *Full = R; *bDone = true; });
	});
	WaitUntil(this, TEXT("CallML"), [bDone] { return *bDone; });
	Step([this, Full, Rig]
	{
		TestTrue(TEXT("CallML returns the full result"), Full->bSuccess && Number(Full->JsonResult, TEXT("mean")) == 6.0);
		Destroy(Rig);
	});
	return true;
}

// Training on its own thread: started, progress and custom events, inputs answered meanwhile, early stop,
// a full run, and an exception reported with its traceback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonMLTrainingTest, "PythonCLI.ML.Training", PythonCLITests::Flags)
bool FPythonMLTrainingTest::RunTest(const FString&)
{
	AddExpectedMessage(TEXT("training failed on purpose"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1, false);
	AddExpectedMessage(TEXT("[python tests/scripts/ml_fixture.py]"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1, false);
	TSharedRef<FRig> Rig = MakeRig(TEXT("tests/scripts/ml_fixture.py"));
	WaitUntil(this, TEXT("setup"), [Rig] { return Rig->M->Setups == 1; });
	Step([Rig] { Rig->ML->BeginTraining(); });
	WaitUntil(this, TEXT("training events"), [Rig]
	{
		return Rig->M->TrainingStarts == 1 && Rig->M->Progress.Num() >= 1 && Rig->M->CustomEvents.Num() >= 2;
	});
	Step([this, Rig]
	{
		UPythonMLTestReceiver* M = Rig->M;
		TestTrue(TEXT("bIsTraining"), Rig->ML->bIsTraining);
		TestTrue(TEXT("progress JSON"), Number(M->Progress[0], TEXT("step")) == 0.0 && Number(M->Progress[0], TEXT("loss")) == 1.0);
		TestTrue(TEXT("custom event with JSON data"), M->CustomEvents[0].Key == TEXT("milestone") && Number(M->CustomEvents[0].Value, TEXT("step")) == 5.0);
		TestTrue(TEXT("custom event with a plain string"), M->CustomEvents[1].Key == TEXT("note") && M->CustomEvents[1].Value == TEXT("plain text"));
		Rig->ML->SendJsonInput(TEXT("{\"a\": 1, \"b\": 1}"), FString());
		Rig->ML->StopTraining();
	});
	WaitUntil(this, TEXT("training complete"), [Rig] { return Rig->M->Completions.Num() == 1; });
	Step([this, Rig]
	{
		const TPair<FString, bool>& Done = Rig->M->Completions[0];
		TestTrue(TEXT("stopped early"), Done.Value && Number(Done.Key, TEXT("steps")) < 400.0);
		TestTrue(TEXT("input answered while training"), Rig->M->FindResult(TEXT("on_json_input")) != nullptr);
		TestFalse(TEXT("not training any more"), Rig->ML->bIsTraining);
		Rig->ML->CallCustomFunction(TEXT("configure"), TEXT("[20]"));
		Rig->ML->BeginTraining();
	});
	WaitUntil(this, TEXT("second training"), [Rig] { return Rig->M->Completions.Num() == 2; });
	Step([this, Rig]
	{
		const TPair<FString, bool>& Done = Rig->M->Completions[1];
		TestTrue(TEXT("a new training runs to the end"), !Done.Value && Number(Done.Key, TEXT("steps")) == 20.0);
		Rig->ML->CallCustomFunction(TEXT("configure"), TEXT("[20, true]"));
		Rig->ML->BeginTraining();
	});
	WaitUntil(this, TEXT("training error"), [Rig] { return Rig->R->Errors.Num() == 1; });
	Step([this, Rig]
	{
		TestTrue(TEXT("training exception reaches OnScriptError with a traceback"), Rig->R->Errors[0].Contains(TEXT("training failed on purpose")) && Rig->R->Tracebacks[0].Contains(TEXT("Traceback")));
		TestFalse(TEXT("not training after the error"), Rig->ML->bIsTraining);
		Destroy(Rig);
	});
	return true;
}

// bTrainOnBeginPlay starts training once set up; EndPlay stops it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonMLTrainOnBeginPlayTest, "PythonCLI.ML.TrainOnBeginPlay", PythonCLITests::Flags)
bool FPythonMLTrainOnBeginPlayTest::RunTest(const FString&)
{
	TSharedRef<FRig> Rig = MakeRig(TEXT("tests/scripts/ml_fixture.py"), [](UPythonMLComponent* C) { C->bTrainOnBeginPlay = true; });
	WaitUntil(this, TEXT("training started by itself"), [Rig] { return Rig->M->TrainingStarts == 1; });
	Step([this, Rig]
	{
		TestTrue(TEXT("set up first"), Rig->M->Setups == 1 && Rig->ML->bIsTraining);
		Destroy(Rig); //EndPlay: stop requested, script unloads after the training thread ends
	});
	return true;
}

// An unchanged TensorFlow-Unreal 1.x style script (camelCase TFPluginAPI, getApi, unreal_engine), named
// without ".py".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonMLLegacyTest, "PythonCLI.ML.LegacyScript", PythonCLITests::Flags)
bool FPythonMLLegacyTest::RunTest(const FString&)
{
	TSharedRef<FRig> Rig = MakeRig(TEXT("tests/scripts/ml_legacy"));
	Rig->ML->SendJsonInput(TEXT("{\"a\": 5, \"b\": 2}"), FString());
	Rig->ML->CallCustomFunction(TEXT("changeOperation"), TEXT("\"-\""));
	Rig->ML->SendJsonInput(TEXT("{\"a\": 5, \"b\": 2}"), FString());
	Rig->ML->BeginTraining();
	WaitUntil(this, TEXT("results and event"), [Rig] { return Rig->M->Results.Num() >= 3 && Rig->M->CustomEvents.Num() >= 1; });
	Step([this, Rig]
	{
		UPythonMLTestReceiver* M = Rig->M;
		TestTrue(TEXT("onJsonInput: 5 + 2"), Number(M->Results[0].Value, TEXT("c")) == 7.0);
		TestTrue(TEXT("custom changeOperation, then 5 - 2"), M->Results[1].Key == TEXT("changeOperation") && Number(M->Results[2].Value, TEXT("c")) == 3.0);
		TestTrue(TEXT("callEvent(name, data, True)"), M->CustomEvents[0].Key == TEXT("TrainingUpdateEvent") && Number(M->CustomEvents[0].Value, TEXT("i")) == 2.0);
		Rig->ML->StopTraining();
	});
	WaitUntil(this, TEXT("stopped"), [Rig] { return Rig->M->Completions.Num() == 1; });
	Step([this, Rig]
	{
		TestTrue(TEXT("shouldStop ended onBeginTraining"), Rig->M->Completions[0].Value);
		Destroy(Rig);
	});
	return true;
}

#endif
