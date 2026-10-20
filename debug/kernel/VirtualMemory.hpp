#pragma once
#include "DebugRecords.h"

#if DEBUG
class DebugManager;
class VirtualMemoryManager;
class ProcessManager;
void DebugVirtualMemoryPublish(DebugManager& manager, VirtualMemoryManager* memory, ProcessManager* processes);
#endif
