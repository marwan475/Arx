#include "layers/Request/SchedulerRequestManager.hpp"

#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Logic/Scheduler.hpp"
#include "layers/Request/RequestLayerFactory.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TaskManager.hpp"
#include "layers/Resource/TimeManager.hpp"

#include <klib/klib.h>

uint64_t SchedulerRequestManager::HandleSchedulingPolicyRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
    {
        return LINUX_EINVAL;
    }

    if (LogicCaps == nullptr || LogicCaps->scheduler == nullptr || ResourceCaps == nullptr || ResourceCaps->taskManager == nullptr)
    {
        return LINUX_ENOSYS;
    }

    const uint64_t number = frame->syscall_number;

    if (number == SYSCALL_sched_get_priority_max || number == SYSCALL_sched_get_priority_min)
    {
        const uint64_t policy = frame->arg0;

        if (policy == SCHEDULING_FIFO || policy == SCHEDULING_RR)
        {
            return number == SYSCALL_sched_get_priority_max ? 99 : 1;
        }

        return policy == SCHEDULING_OTHER || policy == SCHEDULING_DEADLINE ? 0 : LINUX_EINVAL;
    }

    const bool modify = number == SYSCALL_sched_setparam || number == SYSCALL_sched_setscheduler || number == SYSCALL_sched_setattr;
    task_t* task = nullptr;
    uint64_t result = LogicCaps->scheduler->AcquireTask(static_cast<int32_t>(frame->arg0), modify, &task);

    if (result != 0)
    {
        return result;
    }

    result = [&]() -> uint64_t
    {
        scheduling_attributes_t attributes = {};
        uint64_t status = LogicCaps->scheduler->ReadPolicy(task, &attributes);

        if (status != 0)
        {
            return status;
        }

        if (number == SYSCALL_sched_getscheduler)
        {
            return attributes.policy | (attributes.flags & 1 ? 0x40000000U : 0);
        }

        if (number == SYSCALL_sched_getparam)
        {
            return safe_write_user(frame->arg1, &attributes.priority, sizeof(attributes.priority)) ? 0 : LINUX_EFAULT;
        }

        if (number == SYSCALL_sched_rr_get_interval)
        {
            const time_timespec_t interval = {0, attributes.policy == SCHEDULING_RR ? static_cast<int64_t>(SCHEDULING_RR_QUANTUM) : 0};
            return safe_write_user(frame->arg1, &interval, sizeof(interval)) ? 0 : LINUX_EFAULT;
        }

        if (number == SYSCALL_sched_getattr)
        {
            if (frame->arg3 != 0 || frame->arg2 < 48)
            {
                return LINUX_EINVAL;
            }

            const size_t bytes = frame->arg2 < sizeof(attributes) ? frame->arg2 : sizeof(attributes);
            return safe_write_user(frame->arg1, &attributes, bytes) ? 0 : LINUX_EFAULT;
        }

        if (number == SYSCALL_sched_setattr)
        {
            if (frame->arg2 != 0)
            {
                return LINUX_EINVAL;
            }

            uint32_t size = 0;

            if (!safe_read_user(frame->arg1, &size, sizeof(size)))
            {
                return LINUX_EFAULT;
            }

            if (size == 0)
            {
                size = 48;
            }

            if (size < 48 || size > PAGE_SIZE)
            {
                const uint32_t supported = sizeof(attributes);
                return safe_write_user(frame->arg1, &supported, sizeof(supported)) ? static_cast<uint64_t>(-7) : LINUX_EFAULT;
            }

            attributes = {};
            const size_t bytes = size < sizeof(attributes) ? size : sizeof(attributes);

            if (!safe_read_user(frame->arg1, &attributes, bytes))
            {
                return LINUX_EFAULT;
            }

            for (size_t offset = sizeof(attributes); offset < size; ++offset)
            {
                uint8_t extra;

                if (!safe_read_user(frame->arg1 + offset, &extra, 1))
                {
                    return LINUX_EFAULT;
                }

                if (extra != 0)
                {
                    return static_cast<uint64_t>(-7);
                }
            }
        }
        else
        {
            const uint64_t pointer = number == SYSCALL_sched_setscheduler ? frame->arg2 : frame->arg1;
            uint32_t priority;

            if (!safe_read_user(pointer, &priority, sizeof(priority)))
            {
                return LINUX_EFAULT;
            }

            if (number == SYSCALL_sched_setscheduler)
            {
                attributes.policy = frame->arg1 & ~0x40000000ULL;
                attributes.flags = frame->arg1 & 0x40000000ULL ? 1 : 0;
                attributes.runtime = attributes.deadline = attributes.period = 0;
            }

            if (attributes.policy == SCHEDULING_DEADLINE)
            {
                return LINUX_EINVAL;
            }

            attributes.priority = priority;
        }

        return LogicCaps->scheduler->ConfigurePolicy(task, attributes);
    }();

    ResourceCaps->taskManager->ReleaseDispatchClaim(task);
    return result;
}
