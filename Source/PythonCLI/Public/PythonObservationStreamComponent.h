#pragma once

#include "CoreMinimal.h"
#include "PythonStreamComponent.h"
#include "PythonObservationStreamComponent.generated.h"

/**
 * Gameplay observations as rows of named float features. Set features, then CommitRow (once per tick, say);
 * every RowsPerFrame rows go to Python as one (rows, features) float32 frame: frame.array, or frame.rows()
 * for dicts by feature name. With the Lockstep policy, Step() sends the row and returns the handler's
 * result (the action), which makes a deterministic step loop for ML tests.
 */
UCLASS(ClassGroup = "Python", meta = (BlueprintSpawnableComponent))
class PYTHONCLI_API UPythonObservationStreamComponent : public UPythonStreamComponent
{
	GENERATED_BODY()
public:
	UPythonObservationStreamComponent();

	/** The columns of each row, in order (frame.info["features"] in Python). SetFeatureVector("Pos", ...)
	 writes "Pos.x", "Pos.y" and "Pos.z". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream")
	TArray<FString> Features;

	/** Rows per frame: CommitRow sends once this many rows are collected. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Python Stream", meta = (ClampMin = "1"))
	int32 RowsPerFrame = 1;

	/** Values persist between rows until set again. */
	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	void SetFeature(const FString& Name, float Value);

	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	void SetFeatureVector(const FString& Name, FVector Value);

	/** The whole row, in Features order. */
	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	void SetFeatures(const TArray<float>& Values);

	UFUNCTION(BlueprintPure, Category = "Python Stream")
	TArray<float> GetRow() const { return Row; }

	/** Add the current row to the frame being built; sends it once it has RowsPerFrame rows. True if a frame
	 was sent. */
	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	bool CommitRow();

	/** Send the rows collected so far. */
	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	bool Flush();

	/** Lockstep: send the collected rows plus the current one, and wait for the handler's result. */
	UFUNCTION(BlueprintCallable, Category = "Python Stream")
	bool Step(FPythonCallResult& Result);

protected:
	virtual int64 GetFrameBytes() const override;
	virtual TSharedPtr<FJsonObject> MakeStreamInfo() const override;

private:
	int32 FeatureIndex(const FString& Name);
	FPythonStreamFrame MakeFrame(int32 Rows) const;

	TArray<float> Row;
	TArray<float> Batch;
	TMap<FString, int32> Index;
	TSet<FString> Warned;
};
