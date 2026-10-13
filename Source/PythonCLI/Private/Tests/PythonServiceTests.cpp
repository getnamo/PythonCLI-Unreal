// PythonCLI 0.5: services, deferred replies, jobs, streams to a service, image helpers, packaged override.
// The script is Content/Python/tests/scripts/services.py (the harness covers the same protocol engine-free).

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "PythonEnvironment.h"
#include "PythonImageLibrary.h"
#include "PythonJob.h"
#include "PythonProcess.h"
#include "PythonService.h"
#include "PythonStreamComponent.h"
#include "PythonSubsystem.h"
#include "PythonTestHelpers.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHI.h"

using namespace PythonCLITests;

namespace PythonServiceTests
{
	static const TCHAR* Script = TEXT("tests/scripts/services.py");

	struct FServiceCall
	{
		bool bDone = false;
		FPythonCallResult Result;
	};

	static TSharedRef<FServiceCall> Call(UPythonService* S, const FString& Function, const FString& JsonArgs, const TArray<FPyTensor>& Tensors = {})
	{
		TSharedRef<FServiceCall> Out = MakeShared<FServiceCall>();
		S->CallFunction(Function, JsonArgs, Tensors, [Out](const FPythonCallResult& R) { Out->Result = R; Out->bDone = true; });
		return Out;
	}

	static FString RunsDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PythonCLI/TestRuns"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonServiceCallTest, "PythonCLI.Service.Calls", PythonCLITests::Flags)
bool FPythonServiceCallTest::RunTest(const FString&)
{
	using namespace PythonServiceTests;
	UPythonSubsystem* Subsystem = UPythonSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}
	UPythonService* S = Subsystem->GetService(TEXT("test_calls"), Script, EPythonServiceProcess::Shared, {});
	TestTrue(TEXT("same name, same service"), Subsystem->GetService(TEXT("test_calls"), Script, EPythonServiceProcess::Shared, {}) == S && Subsystem->FindService(TEXT("test_calls")) == S);
	struct FState
	{
		TSharedPtr<FServiceCall> Future, Slow, Fast, Reject, Missing;
		int32 Order = 0, SlowOrder = 0, FastOrder = 0;
	};
	const TSharedRef<FState> St = MakeShared<FState>();
	Step([S, St]
	{
		//Called before the script has loaded: the calls wait for it
		St->Future = Call(S, TEXT("future_double"), TEXT("[21]"));
		St->Slow = MakeShared<FServiceCall>();
		St->Fast = MakeShared<FServiceCall>();
		S->CallFunction(TEXT("slow_square"), TEXT("[3, 0.4]"), {}, [St](const FPythonCallResult& R) { St->Slow->Result = R; St->Slow->bDone = true; St->SlowOrder = ++St->Order; });
		S->CallFunction(TEXT("slow_square"), TEXT("[4, 0.05]"), {}, [St](const FPythonCallResult& R) { St->Fast->Result = R; St->Fast->bDone = true; St->FastOrder = ++St->Order; });
		St->Reject = Call(S, TEXT("deferred_reject"), TEXT("[]"));
		St->Missing = Call(S, TEXT("no_such_function"), TEXT("[]"));
	});
	WaitUntil(this, TEXT("service calls"), [St] { return St->Future->bDone && St->Slow->bDone && St->Fast->bDone && St->Reject->bDone && St->Missing->bDone; });
	Step([this, S, St]
	{
		TestTrue(TEXT("service script running"), S->IsRunning());
		TestEqual(TEXT("Future reply"), St->Future->Result.JsonResult, TEXT("42"));
		TestTrue(TEXT("deferred replies"), St->Slow->Result.JsonResult == TEXT("9") && St->Fast->Result.JsonResult == TEXT("16"));
		TestTrue(TEXT("deferred replies arrive out of order"), St->FastOrder == 1 && St->SlowOrder == 2);
		TestTrue(TEXT("reject is a failure"), !St->Reject->Result.bSuccess && St->Reject->Result.Error == TEXT("rejected on purpose"));
		TestFalse(TEXT("unknown function fails"), St->Missing->Result.bSuccess);
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonServiceJobTest, "PythonCLI.Service.Jobs", PythonCLITests::Flags)
bool FPythonServiceJobTest::RunTest(const FString&)
{
	using namespace PythonServiceTests;
	UPythonSubsystem* Subsystem = UPythonSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}
	AddExpectedMessage(TEXT("[python tests/scripts/services.py]"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1, false); //The unknown job
	UPythonService* S = Subsystem->GetService(TEXT("test_jobs"), Script, EPythonServiceProcess::Shared, {});
	struct FState
	{
		TObjectPtr<UPythonJob> Done, Long, Bad, Unknown;
		TSharedPtr<FServiceCall> List;
		int32 Metrics = 0;
		float LastProgress = 0.f;
	};
	const TSharedRef<FState> St = MakeShared<FState>();
	Step([S, St]
	{
		St->Done = S->StartJob(TEXT("count"), TEXT("{\"steps\": 5, \"delay\": 0.001}"), false, RunsDir());
		St->Done->AddToRoot();
		St->Done->OnFinishedNative = [](UPythonJob*) {};
		St->Bad = S->StartJob(TEXT("count"), TEXT("not json"), false, RunsDir());
		St->Bad->AddToRoot();
		St->Unknown = S->StartJob(TEXT("nope"), TEXT("{}"), false, RunsDir());
		St->Unknown->AddToRoot();
		St->List = Call(S, TEXT("_job_list"), TEXT("[]"));
	});
	WaitUntil(this, TEXT("job finished"), [St] { return St->Done->IsFinished() && St->Unknown->IsFinished() && St->List->bDone; });
	Step([this, St]
	{
		UPythonJob* J = St->Done;
		TestEqual(TEXT("job completed"), J->State, EPythonJobState::Completed);
		TestTrue(TEXT("result"), J->Result.IsValid() && J->Result->AsObject()->GetNumberField(TEXT("total")) == 10.0);
		TestEqual(TEXT("progress reaches 1"), J->Progress, 1.f);
		TestEqual(TEXT("last metrics"), J->GetMetric(TEXT("value")), 2.f);
		TestTrue(TEXT("run folder"), FPaths::FileExists(J->RunDir / TEXT("metrics.jsonl")) && FPaths::FileExists(J->RunDir / TEXT("result.json")));
		TestEqual(TEXT("bad parameters fail at once"), St->Bad->State, EPythonJobState::Failed);
		TestTrue(TEXT("unknown job fails"), St->Unknown->State == EPythonJobState::Failed && St->Unknown->Error.Contains(TEXT("has no job 'nope'")));
		TestTrue(TEXT("job list"), St->List->Result.JsonResult.Contains(TEXT("\"count\"")) && St->List->Result.JsonResult.Contains(TEXT("\"steps\"")));
	});
	Step([S, St]
	{
		St->Long = S->StartJob(TEXT("count"), TEXT("{\"steps\": 1000, \"delay\": 0.01}"), false, RunsDir());
		St->Long->AddToRoot();
	});
	WaitUntil(this, TEXT("job running"), [St] { return St->Long->LastMetrics.IsValid() && St->Long->GetMetric(TEXT("value")) >= 1.f; });
	Step([St] { St->Long->Stop(); });
	WaitUntil(this, TEXT("job stopped"), [St] { return St->Long->IsFinished(); }, 10.0);
	Step([this, St]
	{
		TestEqual(TEXT("stopped job"), St->Long->State, EPythonJobState::Stopped);
		TestTrue(TEXT("stopped early"), St->Long->Progress < 0.5f);
		for (UPythonJob* J : { St->Done.Get(), St->Long.Get(), St->Bad.Get(), St->Unknown.Get() })
		{
			J->RemoveFromRoot();
		}
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonServiceDedicatedTest, "PythonCLI.Service.Dedicated", PythonCLITests::Flags)
bool FPythonServiceDedicatedTest::RunTest(const FString&)
{
	using namespace PythonServiceTests;
	UPythonSubsystem* Subsystem = UPythonSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}
	UPythonService* Shared = Subsystem->GetService(TEXT("test_calls"), Script, EPythonServiceProcess::Shared, {});
	UPythonService* Dedicated = Subsystem->GetService(TEXT("test_dedicated"), Script, EPythonServiceProcess::Dedicated, {});
	struct FState
	{
		TSharedPtr<FServiceCall> A, B;
	};
	const TSharedRef<FState> St = MakeShared<FState>();
	Step([Shared, Dedicated, St]
	{
		St->A = Call(Shared, TEXT("future_double"), TEXT("[1]"));
		St->B = Call(Dedicated, TEXT("future_double"), TEXT("[2]"));
	});
	WaitUntil(this, TEXT("calls"), [St] { return St->A->bDone && St->B->bDone; });
	Step([this, Subsystem, Shared, Dedicated, St]
	{
		TestTrue(TEXT("both answer"), St->A->Result.JsonResult == TEXT("2") && St->B->Result.JsonResult == TEXT("4"));
		TestTrue(TEXT("dedicated process is another process"), Dedicated->GetPythonProcessId() != 0 && Dedicated->GetPythonProcessId() != Shared->GetPythonProcessId());
		Subsystem->StopService(TEXT("test_dedicated"));
		TestNull(TEXT("stopped service is forgotten"), Subsystem->FindService(TEXT("test_dedicated")));
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonServiceStreamTest, "PythonCLI.Service.Stream", PythonCLITests::Flags)
bool FPythonServiceStreamTest::RunTest(const FString&)
{
	using namespace PythonServiceTests;
	UPythonSubsystem* Subsystem = UPythonSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), Subsystem))
	{
		return true;
	}
	UPythonService* S = Subsystem->GetService(TEXT("test_calls"), Script, EPythonServiceProcess::Shared, {});
	const TSharedRef<FEnv> Env = MakeShared<FEnv>();
	Env->Create();
	TArray<UPythonStreamComponent*> Streams;
	for (int32 i = 0; i < 2; ++i) //Two components, one service, the same stream name
	{
		AActor* Actor = Env->World->SpawnActor<AActor>();
		Env->Actors.Add(Actor);
		UPythonStreamComponent* St = NewObject<UPythonStreamComponent>(Actor);
		St->StreamName = TEXT("cam");
		St->Policy = EPythonStreamPolicy::Lockstep;
		St->MaxFrameBytes = 1024;
		St->SetTargetService(S, TEXT("process_frame"), TEXT("{\"extra\": 1}"));
		St->RegisterComponent();
		Actor->DispatchBeginPlay();
		Streams.Add(St);
	}
	WaitUntil(this, TEXT("service running"), [S] { return S->IsRunning(); });
	Step([this, Streams, Env]
	{
		for (int32 i = 0; i < Streams.Num(); ++i)
		{
			FPythonCallResult R;
			const bool bOk = Streams[i]->StepTensor(FPyTensor::FromFloats({ 1.f, 2.f, (float)i }), R);
			TestTrue(FString::Printf(TEXT("step %d through the service function: %s"), i, *R.Error), bOk && R.Result.IsValid()
				&& R.Result->AsObject()->GetNumberField(TEXT("sum")) == 3.0 + i && R.Result->AsObject()->GetStringField(TEXT("name")) == TEXT("cam"));
		}
		Env->Destroy();
	});
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonImageTest, "PythonCLI.Image.Tensors", PythonCLITests::Flags)
bool FPythonImageTest::RunTest(const FString&)
{
	//2x2 RGB image, uint8 (H, W, C)
	FPyTensor Image = FPyTensor::FromBytes({ 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255 }, { 2, 2, 3 });
	UTexture2D* Texture = UPythonImageLibrary::TensorToTexture(Image);
	if (!TestNotNull(TEXT("tensor -> texture"), Texture))
	{
		return true;
	}
	FPyImageOptions Options;
	Options.Channels = EPyImageChannels::RGB;
	const FPyTensor Back = UPythonImageLibrary::TextureToTensor(Texture, Options);
	TestTrue(TEXT("texture -> tensor shape"), Back.Shape == TArray<int64>({ 2, 2, 3 }));
	const TArray<float> V = Back.ToFloats();
	TestTrue(TEXT("round trip values"), V.Num() == 12 && V[0] == 1.f && V[1] == 0.f && V[4] == 1.f && V[8] == 1.f && V[9] == 1.f);
	Options.bChannelsFirst = true;
	Options.Normalization = EPyImageNormalization::ImageNet;
	const TArray<float> N = UPythonImageLibrary::TextureToTensor(Texture, Options).ToFloats();
	TestTrue(TEXT("ImageNet normalization, channels first"), N.Num() == 12 && FMath::IsNearlyEqual(N[0], (1.f - 0.485f) / 0.229f, 1e-4f) && FMath::IsNearlyEqual(N[1], -0.485f / 0.229f, 1e-4f));
	const FPyTensor Gray = FPyTensor::FromFloats({ 0.f, 0.5f, 1.f, 0.25f }, { 2, 2 });
	TestTrue(TEXT("update the texture in place"), UPythonImageLibrary::UpdateTextureFromTensor(Texture, Gray));
	TestFalse(TEXT("size mismatch is refused"), UPythonImageLibrary::UpdateTextureFromTensor(Texture, FPyTensor::FromFloats({ 0.f }, { 1, 1 })));

	if (!FApp::CanEverRender() || !GDynamicRHI || FCString::Strcmp(GDynamicRHI->GetName(), TEXT("Null")) == 0)
	{
		AddWarning(TEXT("WriteTensorToRenderTarget needs a real RHI (run without -nullrhi, e.g. -RenderOffscreen): skipped"));
		return true;
	}
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>();
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->InitAutoFormat(4, 4);
	RT->UpdateResourceImmediate(true);
	TestTrue(TEXT("tensor -> render target"), UPythonImageLibrary::WriteTensorToRenderTarget(Image, RT));
	FlushRenderingCommands();
	TestTrue(TEXT("render target resized to the tensor"), RT->SizeX == 2 && RT->SizeY == 2);
	FPyImageOptions Plain;
	const TArray<float> R = UPythonImageLibrary::RenderTargetToTensor(RT, Plain).ToFloats();
	TestTrue(TEXT("render target holds the tensor"), R.Num() == 12 && R[0] == 1.f && R[1] == 0.f && R[5] == 0.f && R[4] == 1.f && R[11] == 1.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonPackagedOverrideTest, "PythonCLI.Environment.PackagedOverride", PythonCLITests::Flags)
bool FPythonPackagedOverrideTest::RunTest(const FString&)
{
	UPythonSubsystem* Subsystem = UPythonSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), Subsystem) || !FPythonEnvironment::HasBundledRuntime())
	{
		return true;
	}
	const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("PythonCLI/test-packaged-requirements.txt"));
	const FString EditorFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectIntermediateDir() / TEXT("PythonCLI/test-editor-requirements.txt"));
	FFileHelper::SaveStringToFile(TEXT("six\n"), *File);
	FFileHelper::SaveStringToFile(TEXT("# an editor file the packaged one replaces (nothing to install)\n"), *EditorFile);
	FPythonEnvironment::RegisterRequirementsFile(EditorFile);
	FPythonEnvironment::RegisterPackagedRequirementsFile(File, EditorFile);
	FPythonEnvironment::SetDefaultPackagedTorchBackend(TEXT("cpu"));
	Subsystem->SetupEnvironment();
	WaitUntil(this, TEXT("environment"), [Subsystem] { return Subsystem->GetEnvironmentState() != EPythonEnvironmentState::SettingUp; }, 600.0);
	Step([this, Subsystem, File, EditorFile]
	{
		FString Requirements, Args;
		FFileHelper::LoadFileToString(Requirements, *FPythonEnvironment::PackagedRequirementsPath());
		FFileHelper::LoadFileToString(Args, *FPythonEnvironment::PackagedArgsPath());
		TestTrue(TEXT("packaged requirements written for the build"), Requirements.Contains(File));
		TestFalse(TEXT("the packaged file replaces its editor file"), Requirements.Contains(EditorFile));
		TestEqual(TEXT("packaged uv arguments"), Args, TEXT("--torch-backend cpu"));
		FPythonEnvironment::UnregisterRequirementsFile(EditorFile);
		FPythonEnvironment::UnregisterPackagedRequirementsFile(File);
		FPythonEnvironment::SetDefaultPackagedTorchBackend(FString());
		Subsystem->SetupEnvironment();
	});
	WaitUntil(this, TEXT("environment"), [Subsystem] { return Subsystem->GetEnvironmentState() != EPythonEnvironmentState::SettingUp; }, 600.0);
	Step([this, File, EditorFile]
	{
		FString Requirements;
		FFileHelper::LoadFileToString(Requirements, *FPythonEnvironment::PackagedRequirementsPath());
		TestFalse(TEXT("unregistered: gone from the packaged requirements"), Requirements.Contains(File));
		IFileManager::Get().Delete(*File);
		IFileManager::Get().Delete(*EditorFile);
	});
	return true;
}

#endif
