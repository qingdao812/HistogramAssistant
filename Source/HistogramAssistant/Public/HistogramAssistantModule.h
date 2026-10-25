#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FHistogramAssistantModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	static const FName TabName;
	static const FName CompareTabName;
};
