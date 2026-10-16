#include <selftests/SelfTestManager.hpp>
#include <selftests/ManagerSuite.hpp>
#include <selftests/TestFixture.hpp>

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

    volatile int      syscallProbeSeen;
    volatile int      syscallAbiValid;
    volatile int      exitSeen;
    volatile uint64_t exitStatus;
    volatile int      hookArmed;
    task_t*           fixtureTask;
    bool              dynamic;
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
    SelfTestManager::Get().RecordFailureDetail(message);
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

        if (!virtualMemoryManager->AllocateAnonymousPage(process->addressSpace, pageVirtualAddress, 3, false))
        {
            return false;
        }

        void* pageBuffer        = virtualMemoryManager->FindRegion(process->addressSpace, pageVirtualAddress)->backing;
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
            virtualMemoryManager->ReleaseAnonymousPage(process->addressSpace, pages[i].virtualAddress);
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

            virtualMemoryManager->ReleaseAnonymousPage(process->addressSpace, pageAddress);
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

    if (frame == nullptr || !g_poststart_elf_ctx.hookArmed || g_poststart_elf_ctx.taskManager->GetCurrentTask() != g_poststart_elf_ctx.fixtureTask)
    {
        return 0;
    }

    if (frame->syscall_number == 0x1338 && g_poststart_elf_ctx.dynamic)
    {
        g_poststart_elf_ctx.syscallProbeSeen = 1;
        g_poststart_elf_ctx.syscallAbiValid  = frame->arg0 == 42;
        *handled                             = true;
        return 0;
    }

    if (frame->syscall_number == SELFTEST_SYSCALL_PROBE)
    {
        uint64_t flags = 0;
        uint32_t mxcsr = 0;
        __asm__ volatile("pushfq; popq %0" : "=r"(flags));
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        g_poststart_elf_ctx.syscallAbiValid = frame->arg0 == 0xA11E && frame->arg1 == 0xBEEF && frame->arg2 == 0xCDEF && frame->arg3 == 0xDEAD && frame->arg4 == 0xBABE && frame->arg5 == 0xFEED && (flags & (1ULL << 9)) != 0 && (flags & (1ULL << 10)) == 0 && mxcsr == 0x1f80
                                              && platform.cpus[arch_cpu_id()].arch_info.syscall_ctx.active == 1;
        // Raw syscalls preserve user SIMD state even when kernel C clobbers it.
        __asm__ volatile("pxor %%xmm0, %%xmm0; pxor %%xmm7, %%xmm7; pxor %%xmm15, %%xmm15" : : : "xmm0", "xmm7", "xmm15");
        g_poststart_elf_ctx.syscallProbeSeen = 1;
        kprintf("Arx kernel: poststart_elf_selftest syscall probe hit (nr=%llu arg0=0x%llx)\n", (unsigned long long) frame->syscall_number, (unsigned long long) frame->arg0);
        *handled = true;
        return 0;
    }

    if (frame->syscall_number == SELFTEST_SYSCALL_EXIT || frame->syscall_number == 231)
    {
        g_poststart_elf_ctx.exitSeen   = 1;
        g_poststart_elf_ctx.exitStatus = frame->arg0;
        kprintf("Arx kernel: poststart_elf_selftest syscall exit hit (status=%llu)\n", (unsigned long long) frame->arg0);

        if (g_poststart_elf_ctx.taskManager != nullptr && g_poststart_elf_ctx.bspTask != nullptr)
        {
            // ExecuteTask restores the destination task's IRQ/preemption state.
            (void) g_poststart_elf_ctx.taskManager->ExecuteTask(g_poststart_elf_ctx.bspTask);
        }

        *handled = true;
        return 0;
    }

    return 0;
}

#endif

static void run_elf_execution_selftest(void* resourceLayerCaps, void* logicLayerCaps, bool dynamic)
{
    const char*        caseName    = dynamic ? "poststartkernel/executable/dynamic_interpreter" : "poststartkernel/executable/user_entry_return";
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

    SelfTestManager::Get().CaseBegin(caseName);

#if !defined(__x86_64__)
    kprintf("Arx kernel: poststart_elf_selftest SKIP: currently x86_64-only syscall fixture\n");
    SelfTestManager::Get().CaseFinish(caseName, 0, 0, SELFTEST_SKIP);
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
        file             = logicCaps->virtualFileSystem->Open(start, dynamic ? "/dynamic_launcher.elf" : "/test_syscall_exit.elf", 0);

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

        virt_addr_space_t* fixtureSpace = dynamic ? resourceCaps->virtualMemoryManager->CreateUserAddressSpace() : currentSpace;
        process                         = fixtureSpace != nullptr ? resourceCaps->processManager->CreateProcess(fixtureSpace) : nullptr;

        if (process != nullptr && dynamic)
        {
            process->ownsAddressSpace = true;
        }
        else if (process == nullptr && dynamic && fixtureSpace != nullptr)
        {
            resourceCaps->virtualMemoryManager->DestroyUserAddressSpace(fixtureSpace);
        }

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

    {
        uint64_t executableEnd = 0;

        for (uint64_t i = 0; i < metadata.segmentCount; ++i)
        {
            const elf_segment_metadata_t& segment = metadata.segments[i];
            const uint64_t                end     = segment.mappedAddress + segment.memorySize;

            if (end > executableEnd)
            {
                executableEnd = end;
            }
        }

        if (process->breakBase != align_up(executableEnd, PAGE_SIZE) || process->programBreak != process->breakBase)
        {
            poststart_elf_test_fail("ELF initializes aligned program break", &fails);
            goto cleanup;
        }

        passes++;
    }

    process->elfMetadata = &metadata;

    if (!setup_user_stack(resourceCaps, process, stackPages, stackPageCount))
    {
        poststart_elf_test_fail("failed to map user stack", &fails);
        goto cleanup;
    }

    stackMapped = true;
    passes++;

    // Exercise the odd metadata parity and inspect actual backing bytes near
    // the page end before building the launch fixture's normal stack.
    {
        const process_user_auxv_entry_t emptyAuxv[] = {{AUXV_AT_PAGESZ, PAGE_SIZE}, {AUXV_AT_NULL, 0}};
        process_user_stack_layout_t     emptyLayout = {};
        emptyLayout.stackBase                       = USER_STACK_TOP - USER_STACK_SIZE;
        emptyLayout.stackSize                       = USER_STACK_SIZE;
        emptyLayout.auxv                            = emptyAuxv;
        emptyLayout.auxvCount                       = 2;
        uint64_t emptyRsp                           = 0;

        if (!resourceCaps->processManager->BuildUserInitialStack(process, &emptyLayout, &emptyRsp) || (emptyRsp & 15) != 0)
        {
            poststart_elf_test_fail("empty initial stack is not aligned", &fails);
            goto cleanup;
        }

        const phys_addr_t physical = resourceCaps->virtualMemoryManager->VirtToPhys(emptyRsp, process->addressSpace);
        const uint64_t*   words    = (const uint64_t*) pa_to_hhdm(physical, platform.numa_nodes[0].zone.hhdm_present, platform.numa_nodes[0].zone.hhdm_offset);

        if (physical == 0 || words[0] != 0 || words[1] != 0 || words[2] != 0 || words[3] != AUXV_AT_PAGESZ || words[4] != PAGE_SIZE || words[5] != AUXV_AT_NULL || words[6] != 0)
        {
            poststart_elf_test_fail("empty initial stack metadata bytes are incorrect", &fails);
            goto cleanup;
        }

        passes++;
    }

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
    g_poststart_elf_ctx.fixtureTask = userTask;
    g_poststart_elf_ctx.dynamic     = dynamic;
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

    if (!g_poststart_elf_ctx.syscallAbiValid || g_poststart_elf_ctx.exitStatus != 0)
    {
        poststart_elf_test_fail("Linux syscall register/FPU/interrupt ABI transition", &fails);
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
        if (!resourceCaps->taskManager->FreeTask(userTask))
        {
            SelfTestManager::Get().AbortUnsafe("integration fixture reclamation failed");
        }
        userTask = nullptr;
    }

    if (stackMapped && !dynamic)
    {
        teardown_user_pages(resourceCaps, process, stackPages, stackPageCount);
    }

    if (elfMapped && !dynamic)
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
        if (!resourceCaps->processManager->FreeProcess(process))
        {
            SelfTestManager::Get().AbortUnsafe("integration fixture reclamation failed");
        }
        process = nullptr;
    }

    SelfTestManager::Get().CaseEnd(caseName, passes, fails);
#endif
}

extern "C" void run_poststart_elf_selftests(void* resourceLayerCaps, void* logicLayerCaps)
{
    auto*        resources = static_cast<ResourceLayerCaps*>(resourceLayerCaps);
    auto*        logic     = static_cast<LogicLayerCaps*>(logicLayerCaps);
    ManagerSuite suite("poststartkernel/ElfMapper", resources != nullptr && logic != nullptr && logic->elfMapper != nullptr && logic->virtualFileSystem != nullptr);
    suite.Run("poststartkernel/ElfMapper/library_backing",
              [&](TestCase& t)
              {
                  TestFixture first(resources);
                  TestFixture second(resources);

                  if (!first.Valid(t) || !second.Valid(t))
                  {
                      return;
                  }

                  file_t* file = logic->virtualFileSystem->Open({}, "/lib/libarx_dynamic_test.so", 0);

                  if (!t.Check(file != nullptr, "required target musl fixture library exists"))
                  {
                      return;
                  }

                  elf_metadata_t a      = {};
                  elf_metadata_t b      = {};
                  bool           loaded = t.Check(logic->elfMapper->ReadElf(file, &a) && logic->elfMapper->ReadElf(file, &b) && logic->elfMapper->LoadLibrary(first.Process, file, &a) && logic->elfMapper->LoadLibrary(second.Process, file, &b), "map same library into independent processes");
                  logic->virtualFileSystem->Close(file);

                  if (loaded)
                  {
                      bool sharedCode   = false;
                      bool privateWrite = false;

                      for (uint64_t index = 0; index < a.segmentCount; ++index)
                      {
                          const auto& segment = a.segments[index];

                          for (uint64_t address = align_down(segment.mappedAddress, PAGE_SIZE); address < segment.mappedAddress + segment.memorySize; address += PAGE_SIZE)
                          {
                              auto* left  = resources->virtualMemoryManager->FindRegion(first.Space, address);
                              auto* right = resources->virtualMemoryManager->FindRegion(second.Space, address - a.loadBias + b.loadBias);

                              if (left == nullptr || right == nullptr || left->shared == nullptr || left->backing != right->backing)
                              {
                                  continue;
                              }

                              if ((left->protection & 4) != 0)
                              {
                                  sharedCode = true;
                                  t.Check(resources->virtualMemoryManager->VirtToPhys(address, first.Space) == resources->virtualMemoryManager->VirtToPhys(address - a.loadBias + b.loadBias, second.Space), "code uses identical physical page after file close");
                              }

                              if ((left->protection & 2) != 0 && !privateWrite)
                              {
                                  uint8_t original = static_cast<uint8_t*>(right->backing)[0];
                                  spinlock_acquire_preempt(&first.Space->operation_lock);
                                  bool copied = vmm_resolve_cow_locked(first.Space, address);
                                  spinlock_release_preempt(&first.Space->operation_lock);
                                  left = resources->virtualMemoryManager->FindRegion(first.Space, address);

                                  if (t.Check(copied && left->backing != right->backing, "writable library page copies on private write"))
                                  {
                                      static_cast<uint8_t*>(left->backing)[0] = original ^ 0xff;
                                      t.Check(static_cast<uint8_t*>(right->backing)[0] == original, "other process retains original library data");
                                      privateWrite = true;
                                  }
                              }
                          }
                      }

                      t.Check(sharedCode, "library has shared executable backing");
                      t.Check(privateWrite, "library has writable backing exercised through COW");
                      t.Equal(first.Process->programBreak, 0, "library mapping leaves heap unchanged");
                  }

                  kfree(a.segments);
                  kfree(b.segments);
              });

    run_elf_execution_selftest(resourceLayerCaps, logicLayerCaps, false);
    run_elf_execution_selftest(resourceLayerCaps, logicLayerCaps, true);
}
