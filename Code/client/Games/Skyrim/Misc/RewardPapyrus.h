#pragma once
#include <Misc/BSScript.h>

// Called while the VM is binding native functions; no ECS/transport mutation here.
void BindRFABRewardFunctions(BSScript::IVirtualMachine*);
