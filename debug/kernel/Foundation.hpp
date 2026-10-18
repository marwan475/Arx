#pragma once

#include "DebugRecords.h"

#if DEBUG

class DebugManager;
void DebugFoundationDeclare(DebugManager& manager);
void DebugFoundationPublishC(DebugManager& manager);
void DebugFoundationPublishCpp(DebugManager& manager);
void DebugFoundationRuntimeUpdate();

#endif
