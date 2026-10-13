// Stream tests (PythonCLI.Streams.*): shared memory and inline frames, flow-control policies, deterministic
// lockstep, audio, render-target readback and recovery after a crash. Python side: tests/scripts/streams.py.
// RenderTarget needs a real RHI: run it without -nullrhi (e.g. -RenderOffscreen); elsewhere it is skipped.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "PythonAudioStreamComponent.h"
#include "PythonBlueprintLibrary.h"
#include "PythonComponent.h"
#include "PythonObservationStreamComponent.h"
#include "PythonProcess.h"
#include "PythonRenderTargetStreamComponent.h"
#include "PythonStream.h"
#include "PythonTestHelpers.h"
#include "Dom/JsonObject.h"
#include "Engine/TextureRenderTarget2D.h"
#include "AudioCaptureCore.h"
#include "AudioDevice.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"

using namespace PythonCLITests;

namespace PythonStreamTests
{
	static const TCHAR* Script = TEXT("tests/scripts/streams.py");

	struct FRig
	{
		TSharedRef<FEnv> Env = MakeShared<FEnv>();
		UPythonTestReceiver* R = nullptr;
		UPythonComponent* Python = nullptr;
		AActor* Actor = nullptr;
		TMap<FString, TSharedPtr<FCall>> Calls;
	};

	static TSharedRef<FRig> MakeRig(bool bShared = true)
	{
		TSharedRef<FRig> Rig = MakeShared<FRig>();
		Rig->Env->Create();
		Rig->Python = Rig->Env->Add(Rig->R, [bShared](UPythonComponent* C)
		{
			C->ScriptParams.Script = Script;
			C->bUseSharedProcess = bShared;
		});
		Rig->Actor = Rig->Python->GetOwner();
		return Rig;
	}

	template <typename T>
	static T* AddStream(FRig& Rig, TFunctionRef<void(T*)> Configure)
	{
		T* S = NewObject<T>(Rig.Actor);
		S->bOpenOnBeginPlay = false; //Opened by the test
		S->PythonComponent = Rig.Python;
		Configure(S);
		S->RegisterComponent();
		return S;
	}

	/** Call `Function` and wait for the result (stored as Rig.Calls[Key]). */
	static void CallAndWait(FAutomationTestBase* Test, const TSharedRef<FRig>& Rig, const FString& Key, const FString& Function, const FString& Args)
	{
		Step([Rig, Key, Function, Args] { Rig->Calls.Add(Key, Call(Rig->Python, Function, Args)); });
		WaitUntil(Test, Function, [Rig, Key] { const TSharedPtr<FCall>* C = Rig->Calls.Find(Key); return C && (*C)->bDone; });
	}

	static TSharedPtr<FJsonObject> ResultObject(const FRig& Rig, const FString& Key)
	{
		const TSharedPtr<FCall>* C = Rig.Calls.Find(Key);
		if (!C || !(*C)->Result.bSuccess || !(*C)->Result.Result.IsValid())
		{
			return nullptr;
		}
		return (*C)->Result.Result->AsObject();
	}

	static void WaitStarted(FAutomationTestBase* Test, const TSharedRef<FRig>& Rig, int32 Count = 1)
	{
		WaitUntil(Test, TEXT("script started"), [Rig, Count] { return Rig->R->Started.Num() >= Count; });
	}

	/** Wait (on this thread; ACKs arrive on the pipe thread) until nothing is in flight. */
	static bool Drain(const TSharedPtr<FPythonStream, ESPMode::ThreadSafe>& Stream, double MaxSeconds = 30.0)
	{
		const double End = FPlatformTime::Seconds() + MaxSeconds;
		while (Stream->GetStats().InFlight > 0)
		{
			if (FPlatformTime::Seconds() > End)
			{
				return false;
			}
			FPlatformProcess::SleepNoStats(0.0005f);
		}
		return true;
	}

	static double Percentile(TArray<double> Values, double P)
	{
		if (Values.Num() == 0)
		{
			return 0.0;
		}
		Values.Sort();
		return Values[FMath::Clamp((int32)(P * (Values.Num() - 1)), 0, Values.Num() - 1)];
	}

	static TArray<int64> Seqs(const TSharedPtr<FJsonObject>& Stats)
	{
		TArray<int64> Out;
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (Stats.IsValid() && Stats->TryGetArrayField(TEXT("seqs"), List))
		{
			for (const TSharedPtr<FJsonValue>& V : *List)
			{
				Out.Add((int64)V->AsNumber());
			}
		}
		return Out;
	}
}

using namespace PythonStreamTests;

// 1080p RGBA8 frames through shared memory, then small inline frames: everything arrives intact and in order.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonStreamThroughputTest, "PythonCLI.Streams.Throughput", PythonCLITests::Flags)
bool FPythonStreamThroughputTest::RunTest(const FString&)
{
	TSharedRef<FRig> Rig = MakeRig();
	WaitStarted(this, Rig);

	constexpr int32 W = 1920, H = 1080, Frames = 300, SmallFrames = 5000;
	Step([this, Rig]
	{
		FPythonStreamConfig Config;
		Config.Owner = Rig->Python->GetOwnerId();
		Config.Script = Script;
		Config.Name = TEXT("frames");
		Config.Policy = EPythonStreamPolicy::Queue;
		Config.Slots = 4;
		Config.SlotBytes = (int64)W * H * 4;
		FString Error;
		TSharedPtr<FPythonStream, ESPMode::ThreadSafe> Stream = FPythonStream::Open(Rig->Python->GetProcess().ToSharedRef(), Config, Error);
		if (!TestTrue(TEXT("stream opened: ") + Error, Stream.IsValid()))
		{
			return;
		}
		TestTrue(TEXT("a 1080p frame uses shared memory"), Stream->UsesSharedMemory());
		double FillSeconds = 0.0;
		const double T0 = FPlatformTime::Seconds();
		for (int32 i = 0; i < Frames; ++i)
		{
			const int32 Slot = Stream->AcquireSlot(10.0);
			if (Slot == INDEX_NONE)
			{
				AddError(FString::Printf(TEXT("no slot for frame %d"), i));
				break;
			}
			const double F0 = FPlatformTime::Seconds();
			uint32* P = reinterpret_cast<uint32*>(Stream->GetSlotData(Slot));
			for (int64 k = 0, N = (int64)W * H; k < N; ++k)
			{
				P[k] = (uint32)i; //Python checks every u32 == frame.seq
			}
			FillSeconds += FPlatformTime::Seconds() - F0;
			FPythonStreamFrame Frame;
			Frame.DType = EPyDType::UInt32;
			Frame.Shape = { (int64)W * H };
			Frame.NumBytes = (int64)W * H * 4;
			Stream->Commit(Slot, Frame);
		}
		TestTrue(TEXT("every slot came back"), Drain(Stream));
		const double Elapsed = FPlatformTime::Seconds() - T0;
		const FPythonStreamStats S = Stream->GetStats();
		TestEqual(TEXT("sent"), S.Sent, (int64)Frames);
		TestEqual(TEXT("acked"), S.Acked, (int64)Frames);
		const FString Report = FString::Printf(TEXT("Shared memory 1080p RGBA8 x%d: %.0f fps, %.2f GB/s including Unreal's fill (%.2f ms/frame) and Python's check; ack latency %.2f ms"),
			Frames, Frames / Elapsed, Frames * (double)W * H * 4 / Elapsed / 1e9, FillSeconds / Frames * 1000.0, S.AckLatencyMs);
		AddInfo(Report);
		UE_LOG(LogTemp, Display, TEXT("%s"), *Report);
		Stream->Close();
	});
	CallAndWait(this, Rig, TEXT("big"), TEXT("get_stats"), TEXT("[\"frames\"]"));
	CallAndWait(this, Rig, TEXT("reset"), TEXT("reset"), TEXT(""));
	Step([this, Rig]
	{
		const TSharedPtr<FJsonObject> St = ResultObject(*Rig, TEXT("big"));
		TestTrue(TEXT("python got every 1080p frame intact and in order"), St.IsValid() && St->GetNumberField(TEXT("frames")) == Frames
			&& St->GetNumberField(TEXT("bad")) == 0 && St->GetNumberField(TEXT("out_of_order")) == 0);

		FPythonStreamConfig Config;
		Config.Owner = Rig->Python->GetOwnerId();
		Config.Script = Script;
		Config.Name = TEXT("frames");
		Config.Policy = EPythonStreamPolicy::Queue;
		Config.Slots = 8;
		Config.SlotBytes = 256;
		FString Error;
		TSharedPtr<FPythonStream, ESPMode::ThreadSafe> Stream = FPythonStream::Open(Rig->Python->GetProcess().ToSharedRef(), Config, Error);
		if (!TestTrue(TEXT("inline stream opened"), Stream.IsValid()))
		{
			return;
		}
		TestFalse(TEXT("small frames travel inline"), Stream->UsesSharedMemory());
		uint32 Data[64];
		const double T0 = FPlatformTime::Seconds();
		for (int32 i = 0; i < SmallFrames; ++i)
		{
			for (uint32& V : Data)
			{
				V = (uint32)i;
			}
			FPythonStreamFrame Frame;
			Frame.DType = EPyDType::UInt32;
			Frame.Shape = { 64 };
			Frame.NumBytes = sizeof(Data);
			if (!Stream->Push(Data, Frame, 10.0))
			{
				AddError(FString::Printf(TEXT("inline frame %d not sent"), i));
				break;
			}
		}
		TestTrue(TEXT("inline slots came back"), Drain(Stream));
		const double Elapsed = FPlatformTime::Seconds() - T0;
		const FString Report = FString::Printf(TEXT("Inline 256-byte frames x%d: %.0f frames/s (ack latency %.3f ms)"), SmallFrames, SmallFrames / Elapsed, Stream->GetStats().AckLatencyMs);
		AddInfo(Report);
		UE_LOG(LogTemp, Display, TEXT("%s"), *Report);
		Stream->Close();
	});
	CallAndWait(this, Rig, TEXT("small"), TEXT("get_stats"), TEXT("[\"frames\"]"));
	Step([this, Rig]
	{
		const TSharedPtr<FJsonObject> St = ResultObject(*Rig, TEXT("small"));
		TestTrue(TEXT("python got every inline frame intact and in order"), St.IsValid() && St->GetNumberField(TEXT("frames")) == SmallFrames
			&& St->GetNumberField(TEXT("bad")) == 0 && St->GetNumberField(TEXT("out_of_order")) == 0);
		Rig->Env->Destroy();
	});
	return true;
}

// Latest: a slow handler; Unreal drops what it can't send, Python skips stale frames, and the newest arrives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonStreamLatestTest, "PythonCLI.Streams.Latest", PythonCLITests::Flags)
bool FPythonStreamLatestTest::RunTest(const FString&)
{
	TSharedRef<FRig> Rig = MakeRig();
	WaitStarted(this, Rig);
	CallAndWait(this, Rig, TEXT("sleep"), TEXT("set_sleep"), TEXT("[0.02]"));
	TSharedRef<FPythonStreamStats> Stats = MakeShared<FPythonStreamStats>();
	Step([this, Rig, Stats]
	{
		FPythonStreamConfig Config;
		Config.Owner = Rig->Python->GetOwnerId();
		Config.Script = Script;
		Config.Name = TEXT("frames");
		Config.Policy = EPythonStreamPolicy::Latest;
		Config.Transport = EPythonStreamTransport::SharedMemory;
		Config.Slots = 3;
		Config.SlotBytes = 4096;
		FString Error;
		TSharedPtr<FPythonStream, ESPMode::ThreadSafe> Stream = FPythonStream::Open(Rig->Python->GetProcess().ToSharedRef(), Config, Error);
		if (!TestTrue(TEXT("stream opened"), Stream.IsValid()))
		{
			return;
		}
		int64 Seq = 0;
		for (int32 i = 0; i < 60; ++i)
		{
			const int32 Slot = Stream->AcquireSlot(0.0);
			if (Slot != INDEX_NONE)
			{
				uint32* P = reinterpret_cast<uint32*>(Stream->GetSlotData(Slot));
				for (int32 k = 0; k < 1024; ++k)
				{
					P[k] = (uint32)Seq;
				}
				FPythonStreamFrame Frame;
				Frame.DType = EPyDType::UInt32;
				Frame.Shape = { 1024 };
				Frame.NumBytes = 4096;
				Stream->Commit(Slot, Frame);
				++Seq;
			}
			FPlatformProcess::SleepNoStats(0.002f);
		}
		TestTrue(TEXT("slots came back"), Drain(Stream));
		*Stats = Stream->GetStats();
		Stream->Close();
	});
	CallAndWait(this, Rig, TEXT("nosleep"), TEXT("set_sleep"), TEXT("[0]"));
	CallAndWait(this, Rig, TEXT("stats"), TEXT("get_stats"), TEXT("[\"frames\"]"));
	Step([this, Rig, Stats]
	{
		const TArray<int64> Got = Seqs(ResultObject(*Rig, TEXT("stats")));
		TArray<int64> Sorted = Got;
		Sorted.Sort();
		AddInfo(FString::Printf(TEXT("Latest: Unreal sent %lld, dropped %lld; Python handled %d"), Stats->Sent, Stats->Dropped, Got.Num()));
		TestTrue(TEXT("Unreal dropped frames while every slot was busy"), Stats->Dropped > 0);
		TestTrue(TEXT("Python skipped stale frames"), Got.Num() > 0 && Got.Num() < Stats->Sent);
		TestTrue(TEXT("handled frames are in order"), Got == Sorted);
		TestTrue(TEXT("the newest frame was handled"), Got.Num() > 0 && Got.Last() == Stats->Sent - 1);
		Rig->Env->Destroy();
	});
	return true;
}

// Observation rows with feature names, batched RowsPerFrame per frame.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonStreamObservationsTest, "PythonCLI.Streams.Observations", PythonCLITests::Flags)
bool FPythonStreamObservationsTest::RunTest(const FString&)
{
	TSharedRef<FRig> Rig = MakeRig();
	UPythonObservationStreamComponent* Obs = AddStream<UPythonObservationStreamComponent>(*Rig, [](UPythonObservationStreamComponent* S)
	{
		S->StreamName = TEXT("obs");
		S->Features = { TEXT("a"), TEXT("b"), TEXT("c") };
		S->RowsPerFrame = 4;
		S->Policy = EPythonStreamPolicy::Queue;
	});
	TWeakObjectPtr<UPythonObservationStreamComponent> Weak(Obs);
	WaitStarted(this, Rig);
	Step([this, Weak]
	{
		if (!TestTrue(TEXT("stream opened"), Weak->OpenStream()))
		{
			return;
		}
		int32 Frames = 0;
		for (int32 i = 0; i < 50; ++i)
		{
			for (int32 r = 0; r < 4; ++r)
			{
				Weak->SetFeature(TEXT("a"), (float)i);
				Weak->SetFeature(TEXT("b"), 1.f);
				Weak->SetFeature(TEXT("c"), -0.5f);
				Frames += Weak->CommitRow() ? 1 : 0;
			}
		}
		TestEqual(TEXT("a frame every 4 rows"), Frames, 50);
		TestTrue(TEXT("slots came back"), Drain(Weak->GetStream()));
	});
	CallAndWait(this, Rig, TEXT("stats"), TEXT("get_stats"), TEXT("[\"obs\"]"));
	Step([this, Rig]
	{
		const TSharedPtr<FJsonObject> St = ResultObject(*Rig, TEXT("stats"));
		if (!TestTrue(TEXT("stats"), St.IsValid()))
		{
			return;
		}
		TestEqual(TEXT("rows"), (int32)St->GetNumberField(TEXT("rows")), 200);
		TestEqual(TEXT("feature names"), PythonWire::ToJsonString(St->TryGetField(TEXT("features"))), FString(TEXT("[\"a\",\"b\",\"c\"]")));
		TestTrue(TEXT("values"), FMath::IsNearlyEqual(St->GetNumberField(TEXT("sum")), 5000.0, 1e-3));
		const TSharedPtr<FJsonObject>* Last = nullptr;
		TestTrue(TEXT("last row by name"), St->TryGetObjectField(TEXT("last_row"), Last) && (*Last)->GetNumberField(TEXT("a")) == 49.0 && (*Last)->GetNumberField(TEXT("c")) == -0.5);
		Rig->Env->Destroy();
	});
	return true;
}

// Lockstep: the game thread steps a little simulation with Python's actions. Same seed, same trajectory,
// also after the script is reloaded; every action answers its own step.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonStreamLockstepTest, "PythonCLI.Streams.Lockstep", PythonCLITests::Flags)
bool FPythonStreamLockstepTest::RunTest(const FString&)
{
	constexpr int32 Steps = 300;
	struct FRun
	{
		TArray<double> X;
		TArray<double> LatencyUs;
		int32 Mismatched = 0;
		FString Error;
	};
	TSharedRef<FRig> Rig = MakeRig();
	UPythonObservationStreamComponent* Obs = AddStream<UPythonObservationStreamComponent>(*Rig, [](UPythonObservationStreamComponent* S)
	{
		S->StreamName = TEXT("step");
		S->Features = { TEXT("step"), TEXT("x"), TEXT("v") };
		S->Policy = EPythonStreamPolicy::Lockstep;
		S->Slots = 1;
		S->StepTimeoutSeconds = 10.f;
	});
	TWeakObjectPtr<UPythonObservationStreamComponent> Weak(Obs);
	TSharedRef<TArray<FRun>> Runs = MakeShared<TArray<FRun>>();

	auto Episode = [Weak, Runs]()
	{
		FRun Run;
		double X = 1.0, V = 0.0;
		const double Dt = 1.0 / 60.0;
		for (int32 k = 0; k < Steps; ++k)
		{
			Weak->SetFeature(TEXT("step"), (float)k);
			Weak->SetFeature(TEXT("x"), (float)X);
			Weak->SetFeature(TEXT("v"), (float)V);
			FPythonCallResult Result;
			const double T0 = FPlatformTime::Seconds();
			const bool bOk = Weak->Step(Result);
			Run.LatencyUs.Add((FPlatformTime::Seconds() - T0) * 1e6);
			double StepEcho = -1.0, Force = 0.0; //What a Blueprint would do with Result.JsonResult
			if (!bOk || !UPythonBlueprintLibrary::GetJsonNumber(Result.JsonResult, TEXT("step"), StepEcho)
				|| !UPythonBlueprintLibrary::GetJsonNumber(Result.JsonResult, TEXT("force"), Force))
			{
				Run.Error = FString::Printf(TEXT("step %d: %s %s"), k, *Result.Error, *Result.JsonResult);
				break;
			}
			Run.Mismatched += (int32)StepEcho != k;
			V += Force * Dt;
			X += V * Dt;
			Run.X.Add(X);
		}
		Runs->Add(MoveTemp(Run));
	};

	WaitStarted(this, Rig);
	Step([this, Weak] { TestTrue(TEXT("stream opened"), Weak->OpenStream()); });
	CallAndWait(this, Rig, TEXT("seed1"), TEXT("reset"), TEXT("[777]"));
	Step(Episode);
	Step([Rig] { Rig->Python->ReloadScript(); });
	WaitStarted(this, Rig, 2);
	CallAndWait(this, Rig, TEXT("seed2"), TEXT("reset"), TEXT("[777]"));
	Step(Episode);
	Step([this, Rig, Runs]
	{
		if (!TestEqual(TEXT("two runs"), Runs->Num(), 2))
		{
			return;
		}
		const FRun& A = (*Runs)[0];
		const FRun& B = (*Runs)[1];
		TestTrue(TEXT("run 1 completed: ") + A.Error, A.Error.IsEmpty() && A.X.Num() == Steps);
		TestTrue(TEXT("run 2 (after reload) completed: ") + B.Error, B.Error.IsEmpty() && B.X.Num() == Steps);
		TestEqual(TEXT("every action answers its own step"), A.Mismatched + B.Mismatched, 0);
		TestTrue(TEXT("identical trajectories (deterministic)"), A.X == B.X);
		TestTrue(TEXT("the controller did something"), A.X.Num() == Steps && FMath::Abs(A.X.Last()) < 0.5);
		const FString Report = FString::Printf(TEXT("Lockstep step round trip (game thread blocked): p50 %.0f us, p90 %.0f us, p99 %.0f us over %d steps"),
			Percentile(A.LatencyUs, 0.5), Percentile(A.LatencyUs, 0.9), Percentile(A.LatencyUs, 0.99), A.LatencyUs.Num());
		AddInfo(Report);
		UE_LOG(LogTemp, Display, TEXT("%s"), *Report);
		Rig->Env->Destroy();
	});
	return true;
}

// Audio pushed in device-sized pieces arrives as ChunkSeconds frames with the right rate and content.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonStreamAudioTest, "PythonCLI.Streams.Audio", PythonCLITests::Flags)
bool FPythonStreamAudioTest::RunTest(const FString&)
{
	TSharedRef<FRig> Rig = MakeRig();
	UPythonAudioStreamComponent* Audio = AddStream<UPythonAudioStreamComponent>(*Rig, [](UPythonAudioStreamComponent* S)
	{
		S->Source = EPythonAudioSource::Manual;
		S->ChunkSeconds = 0.02f;
	});
	TWeakObjectPtr<UPythonAudioStreamComponent> Weak(Audio);
	WaitStarted(this, Rig);
	Step([this, Weak]
	{
		if (!TestTrue(TEXT("stream opened"), Weak->OpenStream()))
		{
			return;
		}
		constexpr int32 Rate = 48000, Piece = 480;
		TArray<float> Samples;
		Samples.SetNumUninitialized(Piece);
		for (int32 Start = 0; Start < Rate; Start += Piece)
		{
			for (int32 i = 0; i < Piece; ++i)
			{
				Samples[i] = FMath::Sin(2.0 * PI * 440.0 * (Start + i) / Rate);
			}
			Weak->PushAudio(Samples, 1, Rate);
			FPlatformProcess::SleepNoStats(0.001f); //Paced a little like a device; audio never waits for a slot
		}
		TestTrue(TEXT("slots came back"), Drain(Weak->GetStream()));
		TestEqual(TEXT("no chunk dropped"), Weak->GetStreamStats().Dropped, (int64)0);
	});
	CallAndWait(this, Rig, TEXT("stats"), TEXT("get_stats"), TEXT("[\"audio\"]"));
	Step([this, Rig]
	{
		const TSharedPtr<FJsonObject> St = ResultObject(*Rig, TEXT("stats"));
		if (!TestTrue(TEXT("stats"), St.IsValid()))
		{
			return;
		}
		TestEqual(TEXT("chunks of 20 ms"), (int32)St->GetNumberField(TEXT("frames")), 50);
		TestEqual(TEXT("samples"), (int32)St->GetNumberField(TEXT("samples")), 48000);
		TestEqual(TEXT("sample rate"), (int32)St->GetNumberField(TEXT("sample_rate")), 48000);
		const double Hz = St->GetNumberField(TEXT("frequency"));
		AddInfo(FString::Printf(TEXT("Python measured %.1f Hz"), Hz));
		TestTrue(TEXT("440 Hz arrived as 440 Hz"), FMath::Abs(Hz - 440.0) < 2.0);
		Rig->Env->Destroy();
	});
	return true;
}

// The main submix's output reaches Python (skipped without an audio device).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonStreamSubmixTest, "PythonCLI.Streams.Submix", PythonCLITests::Flags)
bool FPythonStreamSubmixTest::RunTest(const FString&)
{
	if (!GEngine || !GEngine->GetMainAudioDevice().IsValid())
	{
		AddWarning(TEXT("no audio device: skipped"));
		return true;
	}
	TSharedRef<FRig> Rig = MakeRig();
	UPythonAudioStreamComponent* Audio = AddStream<UPythonAudioStreamComponent>(*Rig, [](UPythonAudioStreamComponent* S)
	{
		S->Source = EPythonAudioSource::Submix;
		S->bDownmixToMono = true;
	});
	TWeakObjectPtr<UPythonAudioStreamComponent> Weak(Audio);
	WaitStarted(this, Rig);
	Step([this, Weak] { TestTrue(TEXT("stream opened and listening"), Weak->OpenStream() && Weak->IsCapturing()); });
	WaitUntil(this, TEXT("submix audio"), [Weak] { return Weak.IsValid() && Weak->GetStreamStats().Acked >= 10; }, 20.0);
	Step([Weak] { Weak->StopCapture(); });
	CallAndWait(this, Rig, TEXT("stats"), TEXT("get_stats"), TEXT("[\"audio\"]"));
	Step([this, Rig]
	{
		const TSharedPtr<FJsonObject> St = ResultObject(*Rig, TEXT("stats"));
		TestTrue(TEXT("python got submix chunks with a sample rate"), St.IsValid() && St->GetNumberField(TEXT("frames")) >= 10 && St->GetNumberField(TEXT("sample_rate")) > 0);
		if (St.IsValid())
		{
			AddInfo(FString::Printf(TEXT("submix: %d chunks at %d Hz, %d channel(s)"), (int32)St->GetNumberField(TEXT("frames")),
				(int32)St->GetNumberField(TEXT("sample_rate")), (int32)St->GetNumberField(TEXT("channels"))));
		}
		Rig->Env->Destroy();
	});
	return true;
}

// The default capture device reaches Python. Opt-in (it opens your microphone): add -PythonCLIMicTest.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonStreamMicrophoneTest, "PythonCLI.Streams.Microphone", PythonCLITests::Flags)
bool FPythonStreamMicrophoneTest::RunTest(const FString&)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("PythonCLIMicTest")))
	{
		AddInfo(TEXT("opt-in (opens the microphone): run with -PythonCLIMicTest"));
		return true;
	}
	{
		Audio::FAudioCapture Probe;
		Audio::FCaptureDeviceInfo Info;
		if (!Probe.GetCaptureDeviceInfo(Info))
		{
			AddWarning(TEXT("no capture device: skipped"));
			return true;
		}
		AddInfo(FString::Printf(TEXT("device '%s': %d Hz, %d channel(s)"), *Info.DeviceName, Info.PreferredSampleRate, Info.InputChannels));
	}
	TSharedRef<FRig> Rig = MakeRig();
	UPythonAudioStreamComponent* Audio = AddStream<UPythonAudioStreamComponent>(*Rig, [](UPythonAudioStreamComponent* S)
	{
		S->Source = EPythonAudioSource::Microphone;
	});
	TWeakObjectPtr<UPythonAudioStreamComponent> Weak(Audio);
	WaitStarted(this, Rig);
	Step([this, Weak] { TestTrue(TEXT("stream opened and capturing"), Weak->OpenStream() && Weak->IsCapturing()); });
	WaitUntil(this, TEXT("microphone audio"), [Weak] { return Weak.IsValid() && Weak->GetStreamStats().Acked >= 10; }, 20.0);
	Step([Weak] { Weak->StopCapture(); });
	CallAndWait(this, Rig, TEXT("stats"), TEXT("get_stats"), TEXT("[\"audio\"]"));
	Step([this, Rig]
	{
		const TSharedPtr<FJsonObject> St = ResultObject(*Rig, TEXT("stats"));
		TestTrue(TEXT("python got microphone chunks with a sample rate"), St.IsValid() && St->GetNumberField(TEXT("frames")) >= 10 && St->GetNumberField(TEXT("sample_rate")) > 0);
		if (St.IsValid())
		{
			AddInfo(FString::Printf(TEXT("microphone: %d chunks at %d Hz, %d channel(s)"), (int32)St->GetNumberField(TEXT("frames")),
				(int32)St->GetNumberField(TEXT("sample_rate")), (int32)St->GetNumberField(TEXT("channels"))));
		}
		Rig->Env->Destroy();
	});
	return true;
}

// A render target read back asynchronously into shared memory; Python sees its pixels.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonStreamRenderTargetTest, "PythonCLI.Streams.RenderTarget", PythonCLITests::Flags)
bool FPythonStreamRenderTargetTest::RunTest(const FString&)
{
	if (!FApp::CanEverRender() || !GDynamicRHI || FCString::Strcmp(GDynamicRHI->GetName(), TEXT("Null")) == 0)
	{
		AddWarning(TEXT("needs a real RHI (run without -nullrhi, e.g. -RenderOffscreen): skipped"));
		return true;
	}
	constexpr int32 Size = 256, Captures = 10;
	TSharedRef<FRig> Rig = MakeRig();
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(Rig->Actor);
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->bForceLinearGamma = true;
	RT->ClearColor = FLinearColor(1.f, 0.5f, 0.f, 1.f);
	RT->InitAutoFormat(Size, Size);
	RT->UpdateResourceImmediate(true);
	UPythonRenderTargetStreamComponent* Cam = AddStream<UPythonRenderTargetStreamComponent>(*Rig, [RT](UPythonRenderTargetStreamComponent* S)
	{
		S->RenderTarget = RT;
		S->bAutoCapture = false;
		S->Policy = EPythonStreamPolicy::Queue;
	});
	TWeakObjectPtr<UPythonRenderTargetStreamComponent> Weak(Cam);
	TSharedRef<int32> Done = MakeShared<int32>(0);
	WaitStarted(this, Rig);
	WaitUntil(this, TEXT("captures"), [Weak, Done]
	{
		if (!Weak.IsValid())
		{
			return true;
		}
		const FPythonStreamStats S = Weak->GetStreamStats();
		if (*Done < Captures && Weak->Capture())
		{
			++*Done;
		}
		Weak->PollReadbacks();
		return S.Acked >= Captures;
	}, 30.0);
	CallAndWait(this, Rig, TEXT("stats"), TEXT("get_stats"), TEXT("[\"camera\"]"));
	Step([this, Rig, Weak]
	{
		const TSharedPtr<FJsonObject> St = ResultObject(*Rig, TEXT("stats"));
		if (!TestTrue(TEXT("stats"), St.IsValid()))
		{
			return;
		}
		const FString First = PythonWire::ToJsonString(St->TryGetField(TEXT("first")));
		const FString Layout = St->GetStringField(TEXT("layout"));
		AddInfo(FString::Printf(TEXT("%d frames, layout %s, shape %s, first pixel (RGB) %s, ack latency %.1f ms"), (int32)St->GetNumberField(TEXT("frames")), *Layout,
			*PythonWire::ToJsonString(St->TryGetField(TEXT("shape"))), *First, Weak.IsValid() ? Weak->GetStreamStats().AckLatencyMs : 0.f));
		TestEqual(TEXT("frames"), (int32)St->GetNumberField(TEXT("frames")), Captures);
		TestEqual(TEXT("shape"), PythonWire::ToJsonString(St->TryGetField(TEXT("shape"))), FString(TEXT("[256,256,4]")));
		TestTrue(TEXT("BGRA or RGBA"), Layout == TEXT("BGRA") || Layout == TEXT("RGBA"));
		TestTrue(TEXT("clear color as RGB (255, ~128, 0)"), First == TEXT("[255,127,0]") || First == TEXT("[255,128,0]"));
		Rig->Env->Destroy();
	});
	return true;
}

// A dedicated process crashes mid-stream; the next StartScript relaunches it and the stream works again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPythonStreamRestartTest, "PythonCLI.Streams.ProcessRestart", PythonCLITests::Flags)
bool FPythonStreamRestartTest::RunTest(const FString&)
{
	TSharedRef<FRig> Rig = MakeRig(false);
	UPythonObservationStreamComponent* Obs = AddStream<UPythonObservationStreamComponent>(*Rig, [](UPythonObservationStreamComponent* S)
	{
		S->StreamName = TEXT("obs");
		S->Features = { TEXT("a"), TEXT("b"), TEXT("c") };
		S->Policy = EPythonStreamPolicy::Queue;
	});
	TWeakObjectPtr<UPythonObservationStreamComponent> Weak(Obs);
	auto Send = [this, Weak](int32 Count)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			Weak->SetFeatures({ 1.f, 2.f, 3.f });
			Weak->CommitRow();
		}
		TestTrue(TEXT("slots came back"), Drain(Weak->GetStream()));
	};
	WaitStarted(this, Rig);
	Step([this, Weak, Send] { TestTrue(TEXT("stream opened"), Weak->OpenStream()); Send(5); });
	Step([Rig] { Rig->Python->CallFunction(TEXT("die"), TEXT(""), {}, nullptr); });
	WaitUntil(this, TEXT("crash reported"), [Rig] { return Rig->R->Errors.Num() > 0; });
	Step([Rig] { Rig->Python->StartScript(); });
	WaitStarted(this, Rig, 2);
	Step([Send] { Send(7); });
	CallAndWait(this, Rig, TEXT("stats"), TEXT("get_stats"), TEXT("[\"obs\"]"));
	Step([this, Rig, Weak]
	{
		const TSharedPtr<FJsonObject> St = ResultObject(*Rig, TEXT("stats"));
		TestTrue(TEXT("the relaunched process got the stream's new frames"), St.IsValid() && (int32)St->GetNumberField(TEXT("rows")) == 7);
		TestEqual(TEXT("nothing dropped"), Weak->GetStreamStats().Dropped, (int64)0);
		Rig->Env->Destroy();
	});
	return true;
}

#endif
