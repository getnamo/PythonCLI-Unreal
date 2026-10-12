#include "PythonCLISettings.h"
#include "PythonSubsystem.h"

UPythonCLISettings::UPythonCLISettings()
{
	SectionName = TEXT("PythonCLI");
}

#if WITH_EDITOR
void UPythonCLISettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName Name = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
	if (Name == GET_MEMBER_NAME_CHECKED(UPythonCLISettings, Requirements) || Name == GET_MEMBER_NAME_CHECKED(UPythonCLISettings, IndexUrl) ||
		Name == GET_MEMBER_NAME_CHECKED(UPythonCLISettings, ExtraIndexUrls) || Name == GET_MEMBER_NAME_CHECKED(UPythonCLISettings, TorchBackend) ||
		Name == GET_MEMBER_NAME_CHECKED(UPythonCLISettings, VenvDirectory))
	{
		if (UPythonSubsystem* Subsystem = UPythonSubsystem::Get())
		{
			Subsystem->SetupEnvironment(false);
		}
	}
}
#endif
