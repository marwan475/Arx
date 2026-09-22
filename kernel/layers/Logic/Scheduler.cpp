#include "layers/Logic/Scheduler.hpp"

#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

extern "C"
{
#include <cpu/cpu.h>
#include <platform.h>
}

Scheduler::Scheduler(ResourceLayerCaps* resourceLayerCaps)
{
	ResourceLayerImportCaps = resourceLayerCaps;
	InitializeBspProcessAndTask();
}

void Scheduler::InitializeBspProcessAndTask()
{
	if (ResourceLayerImportCaps == nullptr)
	{
		return;
	}

	if (ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr)
	{
		return;
	}

	ProcessManager* processManager = ResourceLayerImportCaps->processManager;
	TaskManager*    taskManager    = ResourceLayerImportCaps->taskManager;

	uint8_t cpuId = arch_cpu_id();
	if (cpuId >= BOOT_SMP_MAX_CPUS)
	{
		return;
	}

	process_t* runningProcess = processManager->GetRunningProcess(cpuId);
	task_t*    runningTask    = taskManager->GetRunningTask(cpuId);
	if (runningProcess != nullptr && runningTask != nullptr)
	{
		return;
	}

	bool      createdProcess = false;
	process_t* process       = runningProcess;
	if (process == nullptr)
	{
		virt_addr_space_t* currentSpace = platform.cpus[cpuId].address_space;
		if (currentSpace == nullptr)
		{
			return;
		}

		process = processManager->CreateProcess(currentSpace);
		if (process == nullptr)
		{
			return;
		}

		createdProcess = true;
	}

	task_t* task = runningTask;
	if (task == nullptr)
	{
		task = taskManager->AllocateTask();
		if (task == nullptr)
		{
			if (createdProcess)
			{
				processManager->FreeProcess(process);
			}
			return;
		}

		if (!processManager->AddTask(process, task))
		{
			taskManager->FreeTask(task);
			if (createdProcess)
			{
				processManager->FreeProcess(process);
			}
			return;
		}
	}

	if (!processManager->SetRunningProcess(cpuId, process) || !taskManager->SetRunningTask(cpuId, task))
	{
		if (runningTask == nullptr)
		{
			taskManager->FreeTask(task);
		}
		if (createdProcess)
		{
			processManager->FreeProcess(process);
		}
	}
}

bool Scheduler::ScheduleProcess(uint64_t processId)
{
	if (ResourceLayerImportCaps == nullptr)
	{
		return false;
	}

	if (ResourceLayerImportCaps->processManager == nullptr || ResourceLayerImportCaps->taskManager == nullptr)
	{
		return false;
	}

	ProcessManager* processManager = ResourceLayerImportCaps->processManager;
	TaskManager*    taskManager    = ResourceLayerImportCaps->taskManager;

	size_t count = processManager->GetCapacity();
	if (processId >= count)
	{
		return false;
	}

	process_t* table   = processManager->GetProcesses();
	process_t* process = &table[processId];
	if (!process->allocated)
	{
		return false;
	}

	task_t* firstTask = processManager->GetTasks(process);
	if (firstTask == nullptr)
	{
		return false;
	}

	if (!processManager->ActivateProcessAddressSpace(process))
	{
		return false;
	}

	return taskManager->ExecuteTask(firstTask);
}
