#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PythonCLISettings.generated.h"

/** How Python reaches packaged builds (the editor always uses the project's venv). */
UENUM(BlueprintType)
enum class EPythonPackagedEnvMode : uint8
{
	/** Packaging installs the environment's packages for the target platform (uv, also cross-platform:
	 Linux packages from Windows) and stages them with the build, next to the bundled interpreter.
	 Large with CUDA builds of torch or TensorFlow (gigabytes). */
	Bundle,
	/** The packaged game installs its packages with the bundled uv on first launch (into Saved/PythonCLI).
	 Small builds; needs internet access (or an index you host) on first run. */
	InstallOnFirstRun,
	/** No Python in packaged builds (e.g. models exported to ONNX and run with NNE): scripts don't start. */
	None,
};

/** Project Settings > Plugins > Python CLI. */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Python CLI"))
class PYTHONCLI_API UPythonCLISettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UPythonCLISettings();

	/** Where the project's venv lives. Relative paths are relative to the project directory. It can be
	 regenerated at any time (uv's cache makes that fast), so Intermediate/ is a good place. Keep it short
	 on Windows without long path support: some packages (TensorFlow) install very long paths. */
	UPROPERTY(config, EditAnywhere, Category = "Environment")
	FString VenvDirectory = TEXT("Intermediate/PythonCLI/venv");

	/** Packages to install into the venv, pip requirement syntax (e.g. "numpy", "torch==2.14.*").
	 A requirements.txt in the scripts root (Content/Scripts/) is installed too. */
	UPROPERTY(config, EditAnywhere, Category = "Environment")
	TArray<FString> Requirements;

	/** Optional package index URL (uv --index-url). Empty = PyPI. */
	UPROPERTY(config, EditAnywhere, Category = "Environment")
	FString IndexUrl;

	UPROPERTY(config, EditAnywhere, Category = "Environment")
	TArray<FString> ExtraIndexUrls;

	/** Picks the PyTorch build (uv --torch-backend): e.g. "auto", "cpu", "cu130". Empty = not set. */
	UPROPERTY(config, EditAnywhere, Category = "Environment")
	FString TorchBackend;

	/** Set up (create/update) the venv in the background when the engine starts, so it's ready by the time
	 a script runs. Otherwise it's set up on first use. */
	UPROPERTY(config, EditAnywhere, Category = "Environment")
	bool bSetupEnvironmentOnStartup = true;

	/** Environment variables for the Python process, set before any module is imported
	 (e.g. CUDA_VISIBLE_DEVICES, TF_CPP_MIN_LOG_LEVEL, KERAS_BACKEND). */
	UPROPERTY(config, EditAnywhere, Category = "Process")
	TMap<FString, FString> EnvironmentVariables;

	/** Modules the shared process imports as soon as it starts (e.g. "numpy", "torch"), so the first script
	 doesn't pay for them. The shared process lives as long as the engine, across PIE sessions. */
	UPROPERTY(config, EditAnywhere, Category = "Process")
	TArray<FString> PreloadModules;

	/** Start the shared process in the editor once the environment is ready (warm before the first PIE). */
	UPROPERTY(config, EditAnywhere, Category = "Process")
	bool bStartSharedProcessOnStartup = false;

	/** Seconds scripts get to run on_stop() when a process shuts down. */
	UPROPERTY(config, EditAnywhere, Category = "Process")
	float ShutdownGraceSeconds = 0.5f;

	/** How packaged builds get Python packages. Bundle packages what the editor's environment was last set up
	 with (Intermediate/PythonCLI/requirements.txt). Project scripts (Content/Scripts) are staged either way,
	 unless None. */
	UPROPERTY(config, EditAnywhere, Category = "Packaging")
	EPythonPackagedEnvMode PackagedEnvMode = EPythonPackagedEnvMode::Bundle;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
	/** Changing requirements updates the venv in the background. */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
