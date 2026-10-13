// In-engine tests against the bundled interpreter and Content/Python/runtime.py. Run headless:
//   UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests PythonCLI; Quit" -unattended -nullrhi -nopause
// The engine-free protocol tests are Content/Python/tests/harness.py.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "PythonBlueprintLibrary.h"
#include "PythonComponent.h"
#include "PythonEnvironment.h"
#include "PythonProcess.h"
#include "PythonCLISettings.h"
#include "PythonSubsystem.h"
#include "PythonTestHelpers.h"
#include "PythonTestReceiver.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/Paths.h"

using namespace PythonCLITests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonWirePackTest, "PythonCLI.Wire.PackArgs", PythonCLITests::Flags)
bool FPythonWirePackTest::RunTest(const FString&)
{
	TArray<TSharedPtr<FJsonValue>> Args;
	TArray<uint8> Table;
	FString Error;
	TestTrue(TEXT("array"), PythonWire::PackArgs(TEXT("[1, \"a\"]"), {}, Args, Table, Error) && Args.Num() == 2);
	TestTrue(TEXT("single value is wrapped"), PythonWire::PackArgs(TEXT("{\"x\": 1}"), {}, Args, Table, Error) && Args.Num() == 1 && Args[0]->Type == EJson::Object);
	TestTrue(TEXT("nested array stays one argument"), PythonWire::PackArgs(TEXT("[[1,2]]"), {}, Args, Table, Error) && Args.Num() == 1 && Args[0]->Type == EJson::Array);
	TestTrue(TEXT("empty is no arguments"), PythonWire::PackArgs(TEXT(""), {}, Args, Table, Error) && Args.Num() == 0);
	TestFalse(TEXT("invalid JSON is rejected"), PythonWire::PackArgs(TEXT("[1,"), {}, Args, Table, Error));

	const FPyTensor T = FPyTensor::FromFloats({ 1.f, 2.f, 3.f, 4.f, 5.f, 6.f }, { 2, 3 });
	TestTrue(TEXT("tensor appended"), PythonWire::PackArgs(TEXT("[7]"), { T }, Args, Table, Error) && Args.Num() == 2);
	const TArray<FPyTensor> Back = PythonWire::UnpackTensors(MakeShared<FJsonValueArray>(Args), Table);
	TestTrue(TEXT("tensor round trip through the wire format"), Back.Num() == 1 && Back[0].DType == EPyDType::Float32 && Back[0].Shape == TArray<int64>({ 2, 3 }) && Back[0].Data == T.Data);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonJsonHelpersTest, "PythonCLI.Wire.JsonHelpers", PythonCLITests::Flags)
bool FPythonJsonHelpersTest::RunTest(const FString&)
{
	const FString Json = TEXT("{\"force\": 0.25, \"action\": {\"name\": \"jump\", \"values\": [1, 2.5, -3]}}");
	double Number = 0.0;
	FString Text;
	TArray<float> Floats;
	TestTrue(TEXT("field"), UPythonBlueprintLibrary::GetJsonNumber(Json, TEXT("force"), Number) && Number == 0.25);
	TestTrue(TEXT("nested field + index"), UPythonBlueprintLibrary::GetJsonNumber(Json, TEXT("action.values.1"), Number) && Number == 2.5);
	TestTrue(TEXT("string"), UPythonBlueprintLibrary::GetJsonString(Json, TEXT("action.name"), Text) && Text == TEXT("jump"));
	TestTrue(TEXT("array"), UPythonBlueprintLibrary::GetJsonFloats(Json, TEXT("action.values"), Floats) && Floats == TArray<float>({ 1.f, 2.5f, -3.f }));
	TestTrue(TEXT("root value"), UPythonBlueprintLibrary::GetJsonNumber(TEXT("5"), TEXT(""), Number) && Number == 5.0);
	TestFalse(TEXT("missing path"), UPythonBlueprintLibrary::GetJsonNumber(Json, TEXT("action.values.9"), Number));
	TestFalse(TEXT("wrong type"), UPythonBlueprintLibrary::GetJsonNumber(Json, TEXT("action.name"), Number));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonEnvironmentReadyTest, "PythonCLI.Environment.Ready", PythonCLITests::Flags)
bool FPythonEnvironmentReadyTest::RunTest(const FString&)
{
	UPythonSubsystem* Subsystem = UPythonSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), Subsystem) || !TestTrue(TEXT("bundled runtime present (Tools/FetchRuntime)"), FPythonEnvironment::HasBundledRuntime()))
	{
		return true;
	}
	Subsystem->SetupEnvironment();
	WaitUntil(this, TEXT("environment ready"), [Subsystem] { return Subsystem->GetEnvironmentState() != EPythonEnvironmentState::SettingUp; }, 600.0);
	Step([this, Subsystem]
	{
		TestEqual(TEXT("environment state"), Subsystem->GetEnvironmentState(), EPythonEnvironmentState::Ready);
		TestTrue(TEXT("venv site-packages exists"), FPaths::DirectoryExists(Subsystem->GetEnvironment()->SitePackagesDir()));
		AddInfo(FString::Printf(TEXT("venv: %s (%s)"), *Subsystem->GetEnvironment()->VenvDir(), *FPythonEnvironment::BundledRuntimeVersion()));
	});
	return true;
}

// Requirements go through uv into the venv and are importable by a new process. Needs network (or a uv cache).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonRequirementsTest, "PythonCLI.Environment.Requirements", PythonCLITests::Flags)
bool FPythonRequirementsTest::RunTest(const FString&)
{
	UPythonSubsystem* Subsystem = UPythonSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), Subsystem) || !FPythonEnvironment::HasBundledRuntime())
	{
		return true;
	}
	if (FPythonEnvironment::IsPackagedBuild() && GetDefault<UPythonCLISettings>()->PackagedEnvMode != EPythonPackagedEnvMode::InstallOnFirstRun)
	{
		AddInfo(TEXT("packaged with Bundle: the packages were fixed when the game was packaged"));
		return true;
	}
	//In memory only (not saved to config); restored at the end
	UPythonCLISettings* Settings = GetMutableDefault<UPythonCLISettings>();
	const TArray<FString> Saved = Settings->Requirements;
	Settings->Requirements = { TEXT("numpy") };
	Subsystem->SetupEnvironment();
	WaitUntil(this, TEXT("environment updated"), [Subsystem] { return Subsystem->GetEnvironmentState() != EPythonEnvironmentState::SettingUp; }, 600.0);

	TSharedRef<FEnv> E = MakeShared<FEnv>();
	TSharedRef<TSharedPtr<FCall>> Version = MakeShared<TSharedPtr<FCall>>();
	Step([this, Subsystem, E, Version]
	{
		TestEqual(TEXT("environment ready after installing numpy"), Subsystem->GetEnvironmentState(), EPythonEnvironmentState::Ready);
		E->Create();
		UPythonTestReceiver* R = nullptr;
		UPythonComponent* C = E->Add(R, [](UPythonComponent* C) { C->bUseSharedProcess = false; }); //A fresh process
		*Version = Call(C, TEXT("numpy_version"), TEXT(""));
	});
	WaitUntil(this, TEXT("numpy_version"), [Version] { return Version->IsValid() && (*Version)->bDone; });
	Step([this, Version, E, Settings, Saved, Subsystem]
	{
		TestTrue(TEXT("numpy importable in a new process"), (*Version)->Result.bSuccess);
		AddInfo(FString::Printf(TEXT("numpy %s"), *(*Version)->Result.JsonResult));
		E->Destroy();
		Settings->Requirements = Saved;
		Subsystem->SetupEnvironment(); //Rewrites the marker; installed packages stay (installs are additive)
	});
	WaitUntil(this, TEXT("environment restored"), [Subsystem] { return Subsystem->GetEnvironmentState() != EPythonEnvironmentState::SettingUp; }, 120.0);
	return true;
}

// Events both ways, calls with results and errors, started/stopped events (shared process).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonEventsAndCallsTest, "PythonCLI.Component.EventsAndCalls", PythonCLITests::Flags)
bool FPythonEventsAndCallsTest::RunTest(const FString&)
{
	//divide() raises on purpose: its traceback is logged as errors (asserted below via OnLog)
	AddExpectedMessage(TEXT("[python tests/scripts/fixture.py]"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1, false);
	TSharedRef<FEnv> E = MakeShared<FEnv>();
	E->Create();
	UPythonTestReceiver* R = nullptr;
	UPythonComponent* C = E->Add(R, [](UPythonComponent* C) { C->ScriptParams.Args = { TEXT("x") }; });
	TWeakObjectPtr<UPythonComponent> Weak(C);

	//Sent before the script (or the process) is up: queued and delivered in order
	C->EmitEvent(TEXT("ping"), TEXT("[1, \"two\", {\"three\": 3}]"));
	TSharedRef<FCall> Add = Call(C, TEXT("add"), TEXT("[2, 3]"));
	TSharedRef<FCall> Div = Call(C, TEXT("divide"), TEXT("[1, 0]"));
	TSharedRef<FCall> Missing = Call(C, TEXT("nope"), TEXT(""));

	WaitUntil(this, TEXT("calls"), [Add, Div, Missing] { return Add->bDone && Div->bDone && Missing->bDone; });
	WaitUntil(this, TEXT("pong"), [R] { return R->FindEvent(TEXT("pong")) != nullptr; });
	Step([this, R, Add, Div, Missing, Weak]
	{
		TestEqual(TEXT("OnScriptStarted"), R->Started.Num(), 1);
		const FPythonEventData* StartedWith = R->FindEvent(TEXT("started_with"));
		TestTrue(TEXT("on_start got the script args"), StartedWith && StartedWith->JsonArgs == TEXT("[[\"x\"]]"));
		const FPythonEventData* Pong = R->FindEvent(TEXT("pong"));
		TestEqual(TEXT("event round trip"), Pong ? Pong->JsonArgs : FString(), FString(TEXT("[1,\"two\",{\"three\":3}]")));
		TestTrue(TEXT("add -> 5"), Add->Result.bSuccess && Add->Result.JsonResult == TEXT("5"));
		TestTrue(TEXT("divide by zero fails with a traceback"), !Div->Result.bSuccess && Div->Result.Traceback.Contains(TEXT("ZeroDivisionError")));
		TestTrue(TEXT("missing function fails"), !Missing->Result.bSuccess && Missing->Result.Error.Contains(TEXT("no function")));
		TestTrue(TEXT("traceback also reported as an error log"), R->Logs.ContainsByPredicate([](const FString& L) { return L.StartsWith(TEXT("error:")) && L.Contains(TEXT("ZeroDivisionError")); }));
		Weak->StopScript();
	});
	WaitUntil(this, TEXT("OnScriptStopped"), [R] { return R->Stopped.Num() == 1; });
	Step([this, R]
	{
		const FPythonEventData* Bye = R->FindEvent(TEXT("bye"));
		TestTrue(TEXT("on_stop ran (and could still emit)"), Bye != nullptr);
	});
	Step([E] { E->Destroy(); });
	return true;
}

// Tensors and raw bytes both ways.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonTensorsTest, "PythonCLI.Component.Tensors", PythonCLITests::Flags)
bool FPythonTensorsTest::RunTest(const FString&)
{
	TSharedRef<FEnv> E = MakeShared<FEnv>();
	E->Create();
	UPythonTestReceiver* R = nullptr;
	UPythonComponent* C = E->Add(R, [](UPythonComponent*) {});

	const FPyTensor In = FPyTensor::FromFloats({ 1.5f, -2.f, 3.25f, 4.f, 0.5f, 6.f }, { 2, 3 });
	TSharedRef<FCall> Double = Call(C, TEXT("double"), TEXT(""), { In });
	TArray<uint8> Blob;
	Blob.SetNumUninitialized(1 << 20);
	for (int32 i = 0; i < Blob.Num(); ++i) { Blob[i] = (uint8)(i * 31 + 7); }
	TSharedRef<FCall> Echo = Call(C, TEXT("echo"), TEXT(""), { FPyTensor::FromBytes(Blob) });

	WaitUntil(this, TEXT("calls"), [Double, Echo] { return Double->bDone && Echo->bDone; });
	Step([this, Double, Echo, Blob, E]
	{
		const FPythonCallResult& D = Double->Result;
		TestTrue(TEXT("double succeeded"), D.bSuccess);
		if (D.Tensors.Num() == 1)
		{
			TestEqual(TEXT("dtype"), D.Tensors[0].DType, EPyDType::Float32);
			TestTrue(TEXT("shape [2,3]"), D.Tensors[0].Shape == TArray<int64>({ 2, 3 }));
			TestTrue(TEXT("values doubled"), D.Tensors[0].ToFloats() == TArray<float>({ 3.f, -4.f, 6.5f, 8.f, 1.f, 12.f }));
		}
		else
		{
			AddError(FString::Printf(TEXT("expected 1 tensor, got %d (%s)"), D.Tensors.Num(), *D.JsonResult));
		}
		TestTrue(TEXT("1 MB bytes echoed"), Echo->Result.bSuccess && Echo->Result.Tensors.Num() == 1 && Echo->Result.Tensors[0].Data == Blob);
		E->Destroy();
	});
	return true;
}

// print() and native output, attributed to the component's script.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonOutputTest, "PythonCLI.Component.Output", PythonCLITests::Flags)
bool FPythonOutputTest::RunTest(const FString&)
{
	TSharedRef<FEnv> E = MakeShared<FEnv>();
	E->Create();
	UPythonTestReceiver* R = nullptr;
	UPythonComponent* C = E->Add(R, [](UPythonComponent*) {});
	TSharedRef<FCall> Say = Call(C, TEXT("say"), TEXT("[\"hello unreal\"]"));
	WaitUntil(this, TEXT("print reaches OnLog"), [R] { return R->Logs.Contains(TEXT("info: printed: hello unreal")); });
	Step([this, Say, E]
	{
		TestTrue(TEXT("say returned"), Say->bDone && Say->Result.bSuccess);
		E->Destroy();
	});
	return true;
}

// Two components on the shared process: same process, separate script instances; one ending leaves the other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonSharedIsolationTest, "PythonCLI.Component.SharedIsolation", PythonCLITests::Flags)
bool FPythonSharedIsolationTest::RunTest(const FString&)
{
	TSharedRef<FEnv> E = MakeShared<FEnv>();
	E->Create();
	UPythonTestReceiver* RA = nullptr;
	UPythonTestReceiver* RB = nullptr;
	UPythonComponent* A = E->Add(RA, [](UPythonComponent*) {});
	UPythonComponent* B = E->Add(RB, [](UPythonComponent*) {});
	TWeakObjectPtr<UPythonComponent> WA(A), WB(B);
	for (int32 i = 0; i < 3; ++i)
	{
		A->EmitEvent(TEXT("track"), FString::Printf(TEXT("[%d]"), i));
	}
	TSharedRef<FCall> PidA = Call(A, TEXT("pid"), TEXT(""));
	TSharedRef<FCall> PidB = Call(B, TEXT("pid"), TEXT(""));
	TSharedRef<FCall> CountA = Call(A, TEXT("get_count"), TEXT(""));
	TSharedRef<FCall> CountB = Call(B, TEXT("get_count"), TEXT(""));
	WaitUntil(this, TEXT("calls"), [PidA, PidB, CountA, CountB] { return PidA->bDone && PidB->bDone && CountA->bDone && CountB->bDone; });
	TSharedRef<TSharedPtr<FCall>> AfterEnd = MakeShared<TSharedPtr<FCall>>();
	Step([this, PidA, PidB, CountA, CountB, WA, WB, AfterEnd]
	{
		TestTrue(TEXT("same shared process"), PidA->Result.JsonResult == PidB->Result.JsonResult && !PidA->Result.JsonResult.IsEmpty());
		TestTrue(TEXT("A counted its events"), CountA->Result.Result.IsValid() && CountA->Result.Result->AsObject()->GetNumberField(TEXT("count")) == 3);
		TestTrue(TEXT("B has its own module state"), CountB->Result.Result.IsValid() && CountB->Result.Result->AsObject()->GetNumberField(TEXT("count")) == 0);
		WA->GetOwner()->Destroy(); //A's scripts are unloaded; the process keeps running for B
		*AfterEnd = Call(WB.Get(), TEXT("add"), TEXT("[40, 2]"));
	});
	WaitUntil(this, TEXT("B still answers"), [AfterEnd] { return (*AfterEnd)->bDone; });
	Step([this, AfterEnd, E]
	{
		TestTrue(TEXT("B unaffected by A ending"), (*AfterEnd)->Result.bSuccess && (*AfterEnd)->Result.JsonResult == TEXT("42"));
		E->Destroy();
	});
	return true;
}

// bUseSharedProcess off: own process, separate from the shared one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonDedicatedProcessTest, "PythonCLI.Component.DedicatedProcess", PythonCLITests::Flags)
bool FPythonDedicatedProcessTest::RunTest(const FString&)
{
	TSharedRef<FEnv> E = MakeShared<FEnv>();
	E->Create();
	UPythonTestReceiver* RS = nullptr;
	UPythonTestReceiver* RD = nullptr;
	UPythonComponent* Shared = E->Add(RS, [](UPythonComponent*) {});
	UPythonComponent* Dedicated = E->Add(RD, [](UPythonComponent* C) { C->bUseSharedProcess = false; });
	TSharedRef<FCall> PidS = Call(Shared, TEXT("pid"), TEXT(""));
	TSharedRef<FCall> PidD = Call(Dedicated, TEXT("pid"), TEXT(""));
	WaitUntil(this, TEXT("calls"), [PidS, PidD] { return PidS->bDone && PidD->bDone; });
	Step([this, PidS, PidD, E]
	{
		TestTrue(TEXT("both answered"), PidS->Result.bSuccess && PidD->Result.bSuccess);
		TestNotEqual(TEXT("dedicated process is separate"), PidS->Result.JsonResult, PidD->Result.JsonResult);
		E->Destroy();
	});
	return true;
}

// ReloadScript re-runs the module: fresh state, started again, no stopped event.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonReloadTest, "PythonCLI.Component.Reload", PythonCLITests::Flags)
bool FPythonReloadTest::RunTest(const FString&)
{
	TSharedRef<FEnv> E = MakeShared<FEnv>();
	E->Create();
	UPythonTestReceiver* R = nullptr;
	UPythonComponent* C = E->Add(R, [](UPythonComponent*) {});
	TWeakObjectPtr<UPythonComponent> Weak(C);
	C->EmitEvent(TEXT("track"), TEXT("[1]"));
	C->EmitEvent(TEXT("track"), TEXT("[2]"));
	WaitUntil(this, TEXT("started"), [R] { return R->Started.Num() == 1; });
	Step([Weak] { Weak->ReloadScript(); });
	WaitUntil(this, TEXT("started again"), [R] { return R->Started.Num() == 2; });
	TSharedRef<TSharedPtr<FCall>> Count = MakeShared<TSharedPtr<FCall>>();
	Step([Weak, Count] { *Count = Call(Weak.Get(), TEXT("get_count"), TEXT("")); });
	WaitUntil(this, TEXT("count"), [Count] { return (*Count)->bDone; });
	Step([this, R, Count, E]
	{
		TestTrue(TEXT("old instance ran on_stop"), R->FindEvent(TEXT("bye")) != nullptr);
		TestEqual(TEXT("no OnScriptStopped for a reload"), R->Stopped.Num(), 0);
		TestTrue(TEXT("fresh module state"), (*Count)->Result.Result.IsValid() && (*Count)->Result.Result->AsObject()->GetNumberField(TEXT("count")) == 0);
		E->Destroy();
	});
	return true;
}

// A script that fails to import reports the traceback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonLoadErrorTest, "PythonCLI.Component.LoadError", PythonCLITests::Flags)
bool FPythonLoadErrorTest::RunTest(const FString&)
{
	AddExpectedMessage(TEXT("[python tests/scripts/broken.py]"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1, false);
	TSharedRef<FEnv> E = MakeShared<FEnv>();
	E->Create();
	UPythonTestReceiver* R = nullptr;
	E->Add(R, [](UPythonComponent* C) { C->ScriptParams.Script = TEXT("tests/scripts/broken.py"); });
	WaitUntil(this, TEXT("OnScriptError"), [R] { return R->Errors.Num() > 0; });
	Step([this, R, E]
	{
		TestTrue(TEXT("error names the exception"), R->Errors[0].Contains(TEXT("broken on purpose")));
		TestTrue(TEXT("traceback included"), R->Tracebacks[0].Contains(TEXT("Traceback")));
		TestEqual(TEXT("never reported started"), R->Started.Num(), 0);
		E->Destroy();
	});
	return true;
}

// A native crash in a dedicated process: pending calls fail, OnScriptError fires, and the script starts again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonProcessCrashTest, "PythonCLI.Component.ProcessCrash", PythonCLITests::Flags)
bool FPythonProcessCrashTest::RunTest(const FString&)
{
	TSharedRef<FEnv> E = MakeShared<FEnv>();
	E->Create();
	UPythonTestReceiver* R = nullptr;
	UPythonComponent* C = E->Add(R, [](UPythonComponent* C) { C->bUseSharedProcess = false; });
	TWeakObjectPtr<UPythonComponent> Weak(C);
	WaitUntil(this, TEXT("started"), [R] { return R->Started.Num() == 1; });
	TSharedRef<TSharedPtr<FCall>> Die = MakeShared<TSharedPtr<FCall>>();
	Step([Weak, Die] { *Die = Call(Weak.Get(), TEXT("die"), TEXT("")); });
	WaitUntil(this, TEXT("crash reported"), [Die, R] { return (*Die)->bDone && R->Errors.Num() > 0; });
	Step([this, Die, R, Weak]
	{
		TestFalse(TEXT("pending call failed"), (*Die)->Result.bSuccess);
		TestTrue(TEXT("OnScriptError says the process exited"), R->Errors[0].Contains(TEXT("exited")));
		TestFalse(TEXT("script no longer running"), Weak->IsScriptRunning());
		Weak->StartScript();
	});
	WaitUntil(this, TEXT("started again after the crash"), [R] { return R->Started.Num() == 2; });
	TSharedRef<TSharedPtr<FCall>> Add = MakeShared<TSharedPtr<FCall>>();
	Step([Weak, Add] { *Add = Call(Weak.Get(), TEXT("add"), TEXT("[1, 1]")); });
	WaitUntil(this, TEXT("call after restart"), [Add] { return (*Add)->bDone; });
	Step([this, Add, E]
	{
		TestTrue(TEXT("works after restart"), (*Add)->Result.bSuccess && (*Add)->Result.JsonResult == TEXT("2"));
		E->Destroy();
	});
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
