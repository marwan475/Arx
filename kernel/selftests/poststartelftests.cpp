#include "layers/Logic/ElfMapper.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Logic/VirtualFileSystem.hpp"
#include "layers/Resource/PhysicalMemoryManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/VirtualMemoryManager.hpp"

extern "C"
{
#include <arch/arch.h>
#include <boot/boot.h>
#include <cpu/cpu.h>
#include <klib/klib.h>
#include <memory/vmm.h>
#include <platform.h>
#include <selftests/selftests.h>
}

namespace
{
constexpr uint64_t USER_STACK_TOP  = UINT64_C(0x0000700000000000);
constexpr uint64_t USER_STACK_SIZE = PAGE_SIZE * 8;

constexpr uint64_t SELFTEST_SYSCALL_PROBE = 0x1337;
constexpr uint64_t SELFTEST_SYSCALL_EXIT  = 60;

constexpr uint64_t AUXV_AT_NULL   = 0;
constexpr uint64_t AUXV_AT_PHDR   = 3;
constexpr uint64_t AUXV_AT_PHENT  = 4;
constexpr uint64_t AUXV_AT_PHNUM  = 5;
constexpr uint64_t AUXV_AT_PAGESZ = 6;
constexpr uint64_t AUXV_AT_BASE   = 7;
constexpr uint64_t AUXV_AT_ENTRY  = 9;

struct poststart_elf_test_context_t
{
    TaskManager* taskManager;
    task_t*      bspTask;

    volatile int syscallProbeSeen;
    volatile int exitSeen;
    volatile int hookArmed;
};

static poststart_elf_test_context_t g_poststart_elf_ctx = {};

struct mapped_user_page_t
{
    uint64_t virtualAddress;
    void*    pagePointer;
};

static void poststart_elf_test_fail(const char* message, unsigned long long* failures)
{
    (*failures)++;
    selftest_record_failure_detail(message);
    kprintf("Arx kernel: poststart_elf_selftest FAIL: %s\n", message);
}

static bool setup_user_stack(ResourceLayerCaps* resourceCaps, process_t* process, mapped_user_page_t* pages, uint64_t pageCount)
{
    if (resourceCaps == nullptr || process == nullptr || pages == nullptr || pageCount == 0)
    {
        return false;
    }

    if (resourceCaps->virtualMemoryManager == nullptr || resourceCaps->physicalMemoryManager == nullptr || process->addressSpace == nullptr)
    {
        return false;
    }

    VirtualMemoryManager*  virtualMemoryManager  = resourceCaps->virtualMemoryManager;
    PhysicalMemoryManager* physicalMemoryManager = resourceCaps->physicalMemoryManager;

    uint64_t flags = 0;
    ARCH_PAGE_FLAGS_INIT(flags);
    ARCH_PAGE_FLAG_SET_READ(flags);
    ARCH_PAGE_FLAG_SET_WRITE(flags);
    ARCH_PAGE_FLAG_SET_USER(flags);

    const uint64_t stackStart = USER_STACK_TOP - USER_STACK_SIZE;

    for (uint64_t i = 0; i < pageCount; ++i)
    {
        const uint64_t pageVirtualAddress = stackStart + (i * PAGE_SIZE);
        if (virtualMemoryManager->VirtToPhys(pageVirtualAddress, process->addressSpace) != 0)
        {
            return false;
        }

        void* pageBuffer = physicalMemoryManager->Alloc(PAGE_SIZE);
        if (pageBuffer == nullptr)
        {
            return false;
        }

        memset(pageBuffer, 0, PAGE_SIZE);

        const phys_addr_t pagePhysicalAddress = (phys_addr_t) hhdm_to_pa((uintptr_t) pageBuffer, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
        virtualMemoryManager->MapPage(pageVirtualAddress, pagePhysicalAddress, flags, process->addressSpace);

        pages[i].virtualAddress = pageVirtualAddress;
        pages[i].pagePointer    = pageBuffer;
    }

    return true;
}

static void teardown_user_pages(ResourceLayerCaps* resourceCaps, process_t* process, mapped_user_page_t* pages, uint64_t pageCount)
{
    if (resourceCaps == nullptr || process == nullptr || pages == nullptr)
    {
        return;
    }

    if (resourceCaps->virtualMemoryManager == nullptr || resourceCaps->physicalMemoryManager == nullptr || process->addressSpace == nullptr)
    {
        return;
    }

    VirtualMemoryManager*  virtualMemoryManager  = resourceCaps->virtualMemoryManager;
    PhysicalMemoryManager* physicalMemoryManager = resourceCaps->physicalMemoryManager;

    for (uint64_t i = 0; i < pageCount; ++i)
    {
        if (pages[i].virtualAddress != 0)
        {
            virtualMemoryManager->UnmapPage(pages[i].virtualAddress, process->addressSpace);
        }

        if (pages[i].pagePointer != nullptr)
        {
            physicalMemoryManager->Free(pages[i].pagePointer);
        }

        pages[i].virtualAddress = 0;
        pages[i].pagePointer    = nullptr;
    }
}

static void teardown_elf_segments(ResourceLayerCaps* resourceCaps, process_t* process, elf_metadata_t* metadata)
{
    if (resourceCaps == nullptr || process == nullptr || metadata == nullptr)
    {
        return;
    }

    if (resourceCaps->virtualMemoryManager == nullptr || resourceCaps->physicalMemoryManager == nullptr || process->addressSpace == nullptr)
    {
        return;
    }

    VirtualMemoryManager*  virtualMemoryManager  = resourceCaps->virtualMemoryManager;
    PhysicalMemoryManager* physicalMemoryManager = resourceCaps->physicalMemoryManager;

    for (uint64_t segmentIndex = 0; segmentIndex < metadata->segmentCount; ++segmentIndex)
    {
        const elf_segment_metadata_t& segment = metadata->segments[segmentIndex];
        if (!segment.mapped || segment.memorySize == 0)
        {
            continue;
        }

        const uint64_t mapStart = align_down(segment.mappedAddress, PAGE_SIZE);
        const uint64_t mapEnd   = align_up(segment.mappedAddress + segment.memorySize, PAGE_SIZE);

        for (uint64_t pageAddress = mapStart; pageAddress < mapEnd; pageAddress += PAGE_SIZE)
        {
            const phys_addr_t pagePhysicalAddress = virtualMemoryManager->VirtToPhys(pageAddress, process->addressSpace);
            if (pagePhysicalAddress == 0)
            {
                continue;
            }

            virtualMemoryManager->UnmapPage(pageAddress, process->addressSpace);

            void* hhdmAddress = (void*) pa_to_hhdm((uintptr_t) pagePhysicalAddress, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);
            physicalMemoryManager->Free(hhdmAddress);
        }
    }
}
} // namespace

#if defined(__x86_64__)
extern "C" uint64_t selftest_syscall_dispatch(const arch_syscall_frame_t* frame, bool* handled)
{
    if (handled == nullptr)
    {
        return 0;
    }

    *handled = false;

    if (frame == nullptr || !g_poststart_elf_ctx.hookArmed)
    {
        return 0;
    }

    if (frame->syscall_number == SELFTEST_SYSCALL_PROBE)
    {
        g_poststart_elf_ctx.syscallProbeSeen = 1;
        kprintf("Arx kernel: poststart_elf_selftest syscall probe hit (nr=%llu arg0=0x%llx)\n", (unsigned long long) frame->syscall_number, (unsigned long long) frame->arg0);
        *handled = true;
        return 0;
    }

    if (frame->syscall_number == SELFTEST_SYSCALL_EXIT)
    {
        g_poststart_elf_ctx.exitSeen = 1;
        kprintf("Arx kernel: poststart_elf_selftest syscall exit hit (status=%llu)\n", (unsigned long long) frame->arg0);

        if (g_poststart_elf_ctx.taskManager != nullptr && g_poststart_elf_ctx.bspTask != nullptr)
        {
            // x86 syscall entry masks IF; restore interrupts before switching
            // back to BSP task because task context switch does not restore RFLAGS.
            arch_enable_interrupts();
            (void) g_poststart_elf_ctx.taskManager->ExecuteTask(g_poststart_elf_ctx.bspTask);
        }

        *handled = true;
        return 0;
    }

    return 0;
}
#endif

extern "C" void run_poststart_elf_selftests(void* resourceLayerCaps, void* logicLayerCaps)
{
    unsigned long long passes      = 0;
    unsigned long long fails       = 0;
    file_t*            file        = nullptr;
    process_t*         process     = nullptr;
    task_t*            userTask    = nullptr;
    elf_metadata_t     metadata    = {};
    uint64_t           userRsp     = 0;
    bool               elfMapped   = false;
    bool               stackMapped = false;

    static const char* argvValues[] = {
            "test_syscall_exit.elf",
            "--selftest",
    };

    static const char* envpValues[] = {
            "ARX_SELFTEST=1",
    };

    process_user_auxv_entry_t   auxvValues[7] = {};
    process_user_stack_layout_t stackLayout   = {};

    constexpr uint64_t stackPageCount             = USER_STACK_SIZE / PAGE_SIZE;
    mapped_user_page_t stackPages[stackPageCount] = {};

    selftest_case_begin("poststart_elf_selftest");

#if !defined(__x86_64__)
    kprintf("Arx kernel: poststart_elf_selftest SKIP: currently x86_64-only syscall fixture\n");
    passes++;
    selftest_case_end("poststart_elf_selftest", passes, fails);
    return;
#else
    ResourceLayerCaps* resourceCaps = static_cast<ResourceLayerCaps*>(resourceLayerCaps);
    LogicLayerCaps*    logicCaps    = static_cast<LogicLayerCaps*>(logicLayerCaps);

    if (resourceCaps == nullptr || resourceCaps->processManager == nullptr || resourceCaps->taskManager == nullptr || resourceCaps->virtualMemoryManager == nullptr || resourceCaps->physicalMemoryManager == nullptr)
    {
        poststart_elf_test_fail("missing resource capabilities", &fails);
        goto cleanup;
    }

    if (logicCaps == nullptr || logicCaps->virtualFileSystem == nullptr || logicCaps->scheduler == nullptr || logicCaps->elfMapper == nullptr)
    {
        poststart_elf_test_fail("missing logic capabilities", &fails);
        goto cleanup;
    }

    {
        vfs_path_t start = {};
        file             = logicCaps->virtualFileSystem->Open(start, "/test_syscall_exit.elf", 0);
        if (file == nullptr)
        {
            poststart_elf_test_fail("failed to open /test_syscall_exit.elf", &fails);
            goto cleanup;
        }
        passes++;
    }

    {
        virt_addr_space_t* currentSpace = platform.cpus[arch_cpu_id()].address_space;
        if (currentSpace == nullptr)
        {
            poststart_elf_test_fail("missing current address space", &fails);
            goto cleanup;
        }

        process = resourceCaps->processManager->CreateProcess(currentSpace);
        if (process == nullptr)
        {
            poststart_elf_test_fail("failed to create process", &fails);
            goto cleanup;
        }
        passes++;
    }

    if (!logicCaps->elfMapper->ReadElf(file, &metadata))
    {
        poststart_elf_test_fail("ReadElf failed", &fails);
        goto cleanup;
    }
    passes++;

    if (!logicCaps->elfMapper->LoadExecutable(process, file, &metadata))
    {
        poststart_elf_test_fail("LoadExecutable failed", &fails);
        goto cleanup;
    }
    elfMapped = true;
    passes++;

    process->elfMetadata = &metadata;

    if (!setup_user_stack(resourceCaps, process, stackPages, stackPageCount))
    {
        poststart_elf_test_fail("failed to map user stack", &fails);
        goto cleanup;
    }
    stackMapped = true;
    passes++;

    auxvValues[0] = {AUXV_AT_PAGESZ, PAGE_SIZE};
    auxvValues[1] = {AUXV_AT_ENTRY, metadata.entryPoint};
    auxvValues[2] = {AUXV_AT_PHENT, metadata.programHeaderEntrySize};
    auxvValues[3] = {AUXV_AT_PHNUM, metadata.programHeaderCount};
    auxvValues[4] = {AUXV_AT_PHDR, metadata.loadBias + metadata.programHeaderOffset};
    auxvValues[5] = {AUXV_AT_BASE, metadata.loadBias};
    auxvValues[6] = {AUXV_AT_NULL, 0};

    stackLayout.stackBase = USER_STACK_TOP - USER_STACK_SIZE;
    stackLayout.stackSize = USER_STACK_SIZE;
    stackLayout.argv      = argvValues;
    stackLayout.argc      = (uint64_t) (sizeof(argvValues) / sizeof(argvValues[0]));
    stackLayout.envp      = envpValues;
    stackLayout.envc      = (uint64_t) (sizeof(envpValues) / sizeof(envpValues[0]));
    stackLayout.auxv      = auxvValues;
    stackLayout.auxvCount = 7;

    if (!resourceCaps->processManager->BuildUserInitialStack(process, &stackLayout, &userRsp))
    {
        poststart_elf_test_fail("failed to build argc/argv/envp/auxv user stack with ProcessManager", &fails);
        goto cleanup;
    }
    passes++;

    userTask = resourceCaps->taskManager->CreateUserBootstrapTask(metadata.entryPoint, userRsp, 0, 0);
    if (userTask == nullptr)
    {
        poststart_elf_test_fail("failed to create user bootstrap task", &fails);
        goto cleanup;
    }
    passes++;

    if (!resourceCaps->processManager->AddTask(process, userTask))
    {
        poststart_elf_test_fail("failed to attach user task to process", &fails);
        goto cleanup;
    }
    passes++;

    memset(&g_poststart_elf_ctx, 0, sizeof(g_poststart_elf_ctx));
    g_poststart_elf_ctx.taskManager = resourceCaps->taskManager;
    g_poststart_elf_ctx.bspTask     = resourceCaps->taskManager->GetCurrentTask();
    g_poststart_elf_ctx.hookArmed   = (g_poststart_elf_ctx.bspTask != nullptr) ? 1 : 0;

    if (!g_poststart_elf_ctx.hookArmed)
    {
        poststart_elf_test_fail("missing current BSP task for syscall return", &fails);
        goto cleanup;
    }

    if (!logicCaps->scheduler->ScheduleProcess(process->id))
    {
        poststart_elf_test_fail("failed to schedule ELF user process", &fails);
        goto cleanup;
    }

    if (!g_poststart_elf_ctx.syscallProbeSeen)
    {
        poststart_elf_test_fail("user ELF did not perform expected probe syscall", &fails);
        goto cleanup;
    }

    if (!g_poststart_elf_ctx.exitSeen)
    {
        poststart_elf_test_fail("user ELF did not perform expected exit syscall", &fails);
        goto cleanup;
    }

    passes++;

cleanup:
    g_poststart_elf_ctx.hookArmed = 0;

    if (file != nullptr && logicCaps != nullptr && logicCaps->virtualFileSystem != nullptr)
    {
        (void) logicCaps->virtualFileSystem->Close(file);
        file = nullptr;
    }

    if (userTask != nullptr && resourceCaps != nullptr && resourceCaps->taskManager != nullptr)
    {
        resourceCaps->taskManager->FreeTask(userTask);
        userTask = nullptr;
    }

    if (stackMapped)
    {
        teardown_user_pages(resourceCaps, process, stackPages, stackPageCount);
    }

    if (elfMapped)
    {
        teardown_elf_segments(resourceCaps, process, &metadata);
    }

    if (metadata.segments != nullptr)
    {
        kfree(metadata.segments);
        metadata.segments = nullptr;
    }

    if (process != nullptr)
    {
        process->elfMetadata = nullptr;
    }

    if (process != nullptr && resourceCaps != nullptr && resourceCaps->processManager != nullptr)
    {
        resourceCaps->processManager->FreeProcess(process);
        process = nullptr;
    }

    selftest_case_end("poststart_elf_selftest", passes, fails);
#endif
}
