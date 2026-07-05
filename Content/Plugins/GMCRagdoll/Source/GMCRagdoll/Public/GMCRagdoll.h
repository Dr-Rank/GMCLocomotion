#pragma once

#include "Modules/ModuleManager.h"

class FGMCRagdollModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
