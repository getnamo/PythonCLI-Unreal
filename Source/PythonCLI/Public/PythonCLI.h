#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

PYTHONCLI_API DECLARE_LOG_CATEGORY_EXTERN(LogPythonCLI, Log, All);

class FPythonCLIModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
