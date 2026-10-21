#include "layers/Logic/Scheduler.hpp"

#include "layers/Logic/SynchronizationManager.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"

#include <klib/klib.h>
#include <platform.h>

namespace
{
unsigned scheduling_class(uint32_t policy)
{
    return policy == SCHEDULING_DEADLINE ? 2 : policy == SCHEDULING_FIFO || policy == SCHEDULING_RR ? 1 : 0;
}

task_t* deadline_account(task_t* task)
{
    return task->scheduling->deadlineDonor != nullptr ? task->scheduling->deadlineDonor : task;
}
}

uint64_t Scheduler::ReservationUtilization(uint64_t runtime, uint64_t period) const
{
    return period != 0 ? (runtime * SCHEDULING_UTILIZATION_SCALE + period - 1) / period : 0;
}

int Scheduler::FindReservationCpuLocked(task_t* task, uint64_t mask, uint64_t utilization) const
{
    int selected = -1;
    uint64_t least = UINT64_MAX;
    const auto& previous = *task->scheduling;
    for (uint8_t cpu = 0; cpu < platform.cpu_count; ++cpu)
    {
        if (!(mask & (1ULL << cpu)) || !arch_scheduler_timer_ready(cpu))
        {
            continue;
        }
        uint64_t used = DeadlineUtilization[cpu];
        if (previous.policy == SCHEDULING_DEADLINE && previous.reservationCpu == cpu)
        {
            used -= ReservationUtilization(previous.deadlineRuntime, previous.deadlinePeriod);
        }
        if (used <= SCHEDULING_DEADLINE_CAPACITY && utilization <= SCHEDULING_DEADLINE_CAPACITY - used && used < least)
        {
            selected = cpu;
            least = used;
        }
    }
    return selected;
}

uint64_t Scheduler::ConfigurePolicy(task_t* task, const scheduling_attributes_t& attributes)
{
    if (!arch_task_context_supported())
    {
        return LINUX_EOPNOTSUPP;
    }

    if (task == nullptr || task->scheduling == nullptr || (attributes.flags & ~1ULL) != 0 || attributes.utilizationMin != 0 || attributes.utilizationMax != 0)
    {
        return LINUX_EINVAL;
    }

    const uint32_t policy = attributes.policy;

    if (policy != SCHEDULING_OTHER && policy != SCHEDULING_FIFO && policy != SCHEDULING_RR && policy != SCHEDULING_DEADLINE)
    {
        return LINUX_EOPNOTSUPP;
    }

    const bool realtime = policy == SCHEDULING_FIFO || policy == SCHEDULING_RR;

    if ((realtime && (attributes.priority < 1 || attributes.priority > 99)) || (!realtime && attributes.priority != 0) || attributes.nice < -20 || attributes.nice > 19)
    {
        return LINUX_EINVAL;
    }

    if (policy == SCHEDULING_DEADLINE && (attributes.runtime < 100000ULL || attributes.period < 1000000ULL || attributes.period > 1000000000000ULL || attributes.deadline != attributes.period || attributes.runtime > attributes.period))
    {
        return LINUX_EINVAL;
    }

    if (policy != SCHEDULING_DEADLINE && (attributes.runtime != 0 || attributes.deadline != 0 || attributes.period != 0))
    {
        return LINUX_EINVAL;
    }

    process_t* process = ResourceLayerImportCaps->processManager->GetCurrentProcess();

    if (process == nullptr)
    {
        return LINUX_ESRCH;
    }

    spinlock_acquire_preempt(&process->credentialLock);
    const bool privileged = process->credentials.user.effective == 0;
    spinlock_release_preempt(&process->credentialLock);

    if (!privileged && (policy != SCHEDULING_OTHER || task->scheduling->policy != SCHEDULING_OTHER || attributes.nice < task->baseNice || task->scheduling->resetOnFork != ((attributes.flags & 1) != 0)))
    {
        return static_cast<uint64_t>(-1);
    }

    uint64_t now = 0;

    if (!arch_time_read_monotonic(&now) && policy != SCHEDULING_OTHER)
    {
        return LINUX_EOPNOTSUPP;
    }

    if (realtime)
    {
        for (uint8_t cpu = 0; cpu < platform.cpu_count; ++cpu)
        {
            if ((task->affinityMask & (1ULL << cpu)) && !arch_scheduler_timer_ready(cpu))
            {
                return LINUX_EOPNOTSUPP;
            }
        }
    }

    if (PriorityManager != nullptr && !PriorityManager->LockSchedulingChange(task))
    {
        return static_cast<uint64_t>(-16);
    }

    LockQueues();
    AccountSchedulingLocked(arch_cpu_id(), now);
    int reservation = -1;
    uint64_t utilization = 0;

    if (policy == SCHEDULING_DEADLINE)
    {
        utilization = ReservationUtilization(attributes.runtime, attributes.period);
        reservation = FindReservationCpuLocked(task, task->affinityMask, utilization);

        if (reservation < 0)
        {
            bool clock = false;

            for (uint8_t cpu = 0; cpu < platform.cpu_count; ++cpu)
            {
                clock |= (task->affinityMask & (1ULL << cpu)) && arch_scheduler_timer_ready(cpu);
            }

            UnlockQueues();

            if (PriorityManager != nullptr)
            {
                PriorityManager->UnlockSchedulingChange();
            }

            return clock ? static_cast<uint64_t>(-16) : LINUX_EOPNOTSUPP;
        }
    }

    auto& state = *task->scheduling;

    if (state.policy == SCHEDULING_DEADLINE && state.reservationCpu >= 0)
    {
        DeadlineUtilization[state.reservationCpu] -= ReservationUtilization(state.deadlineRuntime, state.deadlinePeriod);
    }

    ARX_DEBUG_ADD(DebugStats.policyChanges, 1);
    state.policy = state.effectivePolicy = policy;
    state.effectivePriority = attributes.priority;
    state.deadlineDonor = nullptr;
    state.priority = attributes.priority;
    state.resetOnFork = (attributes.flags & 1) != 0;

    state.readyOrder = ++ReadySequence;
    state.roundRobinRemaining = SCHEDULING_RR_QUANTUM;

    state.deadlineRuntime = attributes.runtime;
    state.deadlinePeriod = attributes.period;
    state.deadlineRemaining = attributes.runtime;
    state.deadlineRelease = policy == SCHEDULING_DEADLINE ? now + attributes.period : 0;
    state.deadlineAbsolute = state.deadlineRelease;
    state.reservationCpu = reservation;

    if (reservation >= 0)
    {
        DeadlineUtilization[reservation] += utilization;
        __atomic_store_n(&task->migrationEnabled, true, __ATOMIC_RELEASE);
    }

    __atomic_store_n(&task->baseNice, attributes.nice, __ATOMIC_RELEASE);
    UnlockQueues();

    if (PriorityManager != nullptr)
    {
        PriorityManager->UnlockSchedulingChange();
        PriorityManager->RefreshPriorities();
    }

    RequestReschedule(AvailableCpuMask());

    // A reservation changing CPUs must take effect before returning success.
    return reservation >= 0 ? SetAffinity(task, task->affinityMask) : 0;
}

uint64_t Scheduler::ReadPolicy(task_t* task, scheduling_attributes_t* attributes)
{
    if (task == nullptr || attributes == nullptr)
    {
        return LINUX_ESRCH;
    }
    LockQueues();
    const auto& state = *task->scheduling;
    *attributes = {sizeof(*attributes), state.policy, state.resetOnFork ? 1ULL : 0ULL, task->baseNice, state.priority, state.deadlineRuntime, state.deadlinePeriod, state.deadlinePeriod, 0, 0};
    UnlockQueues();
    return 0;
}

uint64_t Scheduler::ValidateClonePolicy(task_t* parent) const
{
    return parent->scheduling->policy == SCHEDULING_DEADLINE && !parent->scheduling->resetOnFork ? LINUX_EAGAIN : 0;
}

void Scheduler::InheritPolicy(task_t* parent, task_t* child)
{
    LockQueues();
    auto& destination = *child->scheduling;
    const auto& source = *parent->scheduling;
    if (!source.resetOnFork)
    {
        destination.policy = source.policy;
        destination.priority = source.priority;
    }
    else
    {
        child->baseNice = child->baseNice < 0 ? 0 : child->baseNice;
        child->effectiveNice = child->baseNice;
    }
    destination.effectivePolicy = destination.policy;
    destination.effectivePriority = destination.priority;
    destination.readyOrder = 0;
    destination.roundRobinRemaining = SCHEDULING_RR_QUANTUM;
    UnlockQueues();
}

void Scheduler::ApplyPriorityDonation(task_t* task, int32_t nice, uint32_t policy, uint32_t priority, task_t* donor)
{
    LockQueues();
    auto& state = *task->scheduling;
    ARX_DEBUG_RUN(
        const auto& previous = *task->scheduling;
        task_t* effectiveDonor = donor == task ? nullptr : donor;

        if (task->effectiveNice != nice || previous.effectivePolicy != policy
            || previous.effectivePriority != priority || previous.deadlineDonor != effectiveDonor)
        {
            ARX_DEBUG_ADD(DebugStats.donations, 1);
        }
    );

    __atomic_store_n(&task->effectiveNice, nice, __ATOMIC_RELEASE);
    __atomic_store_n(&state.effectivePriority, priority, __ATOMIC_RELEASE);
    __atomic_store_n(&state.deadlineDonor, donor == task ? nullptr : donor, __ATOMIC_RELEASE);
    __atomic_store_n(&state.effectivePolicy, policy, __ATOMIC_RELEASE);
    UnlockQueues();
    (void) RunSchedulingAlgorithm(arch_cpu_id(), scheduling_reason_t::PRIORITY, nullptr, task);
}

void Scheduler::RetirePolicy(task_t* task)
{
    LockQueues();
    auto& state = *task->scheduling;
    if (state.policy == SCHEDULING_DEADLINE && state.reservationCpu >= 0)
    {
        DeadlineUtilization[state.reservationCpu] -= ReservationUtilization(state.deadlineRuntime, state.deadlinePeriod);
    }
    state.policy = state.effectivePolicy = SCHEDULING_OTHER;
    state.reservationCpu = -1;
    state.deadlineDonor = nullptr;
    for (size_t cpu = 0; cpu < platform.cpu_count; ++cpu)
    {
        if (ExecutionTask[cpu] == task)
        {
            ExecutionTask[cpu] = nullptr;
        }
    }
    UnlockQueues();
}

void Scheduler::ReplenishReservationsLocked(uint64_t now)
{
    task_t* tasks = ResourceLayerImportCaps->taskManager->GetTasks();
    for (size_t i = 0; i < TaskManager::MAX_TASKS; ++i)
    {
        if (!ResourceLayerImportCaps->taskManager->RetainForDispatch(&tasks[i]))
        {
            continue;
        }
        auto& state = *tasks[i].scheduling;
        if (state.policy == SCHEDULING_DEADLINE && now >= state.deadlineRelease)
        {
            const uint64_t periods = (now - state.deadlineRelease) / state.deadlinePeriod + 1;
            state.deadlineRelease += periods * state.deadlinePeriod;
            state.deadlineAbsolute = state.deadlineRelease;
            state.deadlineRemaining = state.deadlineRuntime;
        }
        ResourceLayerImportCaps->taskManager->ReleaseDispatchClaim(&tasks[i]);
    }
}

void Scheduler::AccountSchedulingLocked(uint8_t cpu, uint64_t now)
{
    task_t* current = ResourceLayerImportCaps->taskManager->GetRunningTask(cpu);

    if (RealtimeWindow[cpu] == 0)
    {
        RealtimeWindow[cpu] = now;
    }
    else if (now - RealtimeWindow[cpu] >= SCHEDULING_RT_WINDOW)
    {
        RealtimeWindow[cpu] += ((now - RealtimeWindow[cpu]) / SCHEDULING_RT_WINDOW) * SCHEDULING_RT_WINDOW;
        RealtimeConsumed[cpu] = 0;
    }

    if (current != nullptr && current == ExecutionTask[cpu] && now >= ExecutionStamp[cpu] && current->scheduling != nullptr)
    {
        const uint64_t elapsed = now - ExecutionStamp[cpu];
        auto& state = *current->scheduling;

        if (state.effectivePolicy == SCHEDULING_DEADLINE)
        {
            auto& budget = *deadline_account(current)->scheduling;
            uint64_t charged = elapsed;

            if (now >= budget.deadlineRelease)
            {
                const uint64_t periods = (now - budget.deadlineRelease) / budget.deadlinePeriod + 1;
                budget.deadlineRelease += periods * budget.deadlinePeriod;
                budget.deadlineAbsolute = budget.deadlineRelease;
                budget.deadlineRemaining = budget.deadlineRuntime;
                const uint64_t start = budget.deadlineRelease - budget.deadlinePeriod;
                charged = now - (ExecutionStamp[cpu] > start ? ExecutionStamp[cpu] : start);
            }

            budget.deadlineRemaining = charged < budget.deadlineRemaining ? budget.deadlineRemaining - charged : 0;
        }
        else if (scheduling_class(state.effectivePolicy) == 1)
        {
            const uint64_t start = ExecutionStamp[cpu] > RealtimeWindow[cpu] ? ExecutionStamp[cpu] : RealtimeWindow[cpu];
            RealtimeConsumed[cpu] += now - start;
        }

        if (state.policy == SCHEDULING_RR)
        {
            state.roundRobinRemaining = elapsed < state.roundRobinRemaining ? state.roundRobinRemaining - elapsed : 0;
        }

        if (state.policy == SCHEDULING_OTHER)
        {
            current->scheduleRuntime += (elapsed / 1000000ULL) * (1ULL << ((current->baseNice + 20) / 4));
        }
    }

    ExecutionStamp[cpu] = now;
    ExecutionTask[cpu] = current;
}

void Scheduler::AccountSchedulingCurrent(bool beginExecution)
{
    uint64_t now = 0;
    if (!arch_time_read_monotonic(&now))
    {
        return;
    }
    LockQueues();
    const uint8_t cpu = arch_cpu_id();
    if (beginExecution)
    {
        ExecutionTask[cpu] = ResourceLayerImportCaps->taskManager->GetRunningTask(cpu);
        ExecutionStamp[cpu] = now;
    }
    else
    {
        AccountSchedulingLocked(cpu, now);
    }
    UnlockQueues();
}

bool Scheduler::PolicyEligible(task_t* task, uint8_t cpu) const
{
    const auto& state = *task->scheduling;
    if (state.effectivePolicy == SCHEDULING_DEADLINE)
    {
        const auto& donor = *deadline_account(task)->scheduling;
        return donor.deadlineRemaining != 0 && donor.reservationCpu == cpu;
    }
    if (scheduling_class(state.effectivePolicy) == 1)
    {
        const uint64_t capacity = (SCHEDULING_UTILIZATION_SCALE - DeadlineUtilization[cpu]) * 95 * SCHEDULING_RT_WINDOW / 100 / SCHEDULING_UTILIZATION_SCALE;
        return RealtimeConsumed[cpu] < capacity;
    }
    return true;
}

bool Scheduler::MoreUrgent(task_t* task, task_t* selected, uint8_t cpu) const
{
    (void) cpu;

    if (selected == nullptr)
    {
        return true;
    }

    const auto& candidate = *task->scheduling;
    const auto& previous = *selected->scheduling;
    const unsigned candidateClass = scheduling_class(candidate.effectivePolicy);
    const unsigned previousClass = scheduling_class(previous.effectivePolicy);

    if (candidateClass != previousClass)
    {
        return candidateClass > previousClass;
    }

    if (candidateClass == 2)
    {
        const uint64_t deadline = deadline_account(task)->scheduling->deadlineAbsolute;
        const uint64_t other = deadline_account(selected)->scheduling->deadlineAbsolute;
        return deadline != other ? deadline < other : candidate.readyOrder < previous.readyOrder;
    }

    if (candidateClass == 1)
    {
        return candidate.effectivePriority != previous.effectivePriority ? candidate.effectivePriority > previous.effectivePriority : candidate.readyOrder < previous.readyOrder;
    }

    const bool boosted = task->effectiveNice < task->baseNice;
    const bool previousBoosted = selected->effectiveNice < selected->baseNice;

    if (boosted != previousBoosted)
    {
        return boosted;
    }

    if (boosted && task->effectiveNice != selected->effectiveNice)
    {
        return task->effectiveNice < selected->effectiveNice;
    }

    return task->scheduleRuntime < selected->scheduleRuntime;
}

uint64_t Scheduler::NextSchedulingEventLocked(task_t* task, uint8_t cpu, uint64_t now) const
{
    uint64_t next = now + SCHEDULING_RR_QUANTUM;
    task_t* tasks = ResourceLayerImportCaps->taskManager->GetTasks();

    for (size_t i = 0; i < TaskManager::MAX_TASKS; ++i)
    {
        if (!ResourceLayerImportCaps->taskManager->RetainForDispatch(&tasks[i]))
        {
            continue;
        }

        if (tasks[i].scheduling->policy == SCHEDULING_DEADLINE && tasks[i].scheduling->reservationCpu == cpu)
        {
            const uint64_t release = tasks[i].scheduling->deadlineRelease;

            if (release < next)
            {
                next = release;
            }
        }

        ResourceLayerImportCaps->taskManager->ReleaseDispatchClaim(&tasks[i]);
    }

    if (task != nullptr)
    {
        const auto& state = *task->scheduling;
        uint64_t remaining = SCHEDULING_RR_QUANTUM;

        if (state.effectivePolicy == SCHEDULING_DEADLINE)
        {
            remaining = deadline_account(task)->scheduling->deadlineRemaining;
        }
        else if (scheduling_class(state.effectivePolicy) == 1)
        {
            const uint64_t capacity = (SCHEDULING_UTILIZATION_SCALE - DeadlineUtilization[cpu]) * 95 * SCHEDULING_RT_WINDOW / 100 / SCHEDULING_UTILIZATION_SCALE;
            remaining = RealtimeConsumed[cpu] < capacity ? capacity - RealtimeConsumed[cpu] : 0;

            if (state.policy == SCHEDULING_RR && state.roundRobinRemaining < remaining)
            {
                remaining = state.roundRobinRemaining;
            }
        }

        if (now + remaining < next)
        {
            next = now + remaining;
        }
    }

    const uint64_t realtimeRelease = RealtimeWindow[cpu] + SCHEDULING_RT_WINDOW;

    if (realtimeRelease > now && realtimeRelease < next)
    {
        next = realtimeRelease;
    }

    return next;
}

uint64_t Scheduler::UserPriority(uint32_t selector, uint32_t who, bool modify, int32_t nice)
{
    if (selector > 2)
    {
        return LINUX_EINVAL;
    }

    if (selector == 0)
    {
        task_t* task = nullptr;
        uint64_t status = AcquireTask(who, modify, &task);

        if (status != 0)
        {
            return status;
        }

        status = modify ? SetUserNice(task, nice) : 20 - __atomic_load_n(&task->baseNice, __ATOMIC_ACQUIRE);
        ResourceLayerImportCaps->taskManager->ReleaseDispatchClaim(task);
        return status;
    }

    auto processes = ResourceLayerImportCaps->processManager;
    auto tasks = ResourceLayerImportCaps->taskManager;
    process_t* caller = processes->GetCurrentProcess();

    if (caller == nullptr)
    {
        return LINUX_ESRCH;
    }

    if (who == 0)
    {
        spinlock_acquire_preempt(&caller->credentialLock);
        who = selector == 1 ? caller->groupId : caller->credentials.user.real;
        spinlock_release_preempt(&caller->credentialLock);
    }

    uint64_t result = modify ? 0 : LINUX_ESRCH;
    bool found = false;

    for (size_t i = 0; i < TaskManager::MAX_TASKS; ++i)
    {
        task_t* task = &tasks->GetTasks()[i];

        if (!tasks->RetainForDispatch(task))
        {
            continue;
        }

        process_t* owner = task->process;
        bool matches = false;

        if (owner != nullptr && !owner->exited)
        {
            spinlock_acquire_preempt(&owner->credentialLock);
            matches = selector == 1 ? owner->groupId == who : owner->credentials.user.real == who;
            spinlock_release_preempt(&owner->credentialLock);
        }

        if (matches)
        {
            found = true;

            if (modify)
            {
                task_t* permitted = nullptr;
                uint64_t status = AcquireTask(task->id, true, &permitted);

                if (status == 0)
                {
                    status = SetUserNice(permitted, nice);
                    tasks->ReleaseDispatchClaim(permitted);
                }

                if (status != 0)
                {
                    result = status;
                }
            }
            else
            {
                const uint64_t priority = 20 - __atomic_load_n(&task->baseNice, __ATOMIC_ACQUIRE);

                if (result == LINUX_ESRCH || priority > result)
                {
                    result = priority;
                }
            }
        }

        tasks->ReleaseDispatchClaim(task);
    }

    return found ? result : LINUX_ESRCH;
}
