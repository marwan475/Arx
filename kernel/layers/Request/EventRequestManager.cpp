#include "layers/Request/EventRequestManager.hpp"

#include "layers/Logic/EventManager.hpp"
#include "layers/Logic/LogicLayerFactory.hpp"
#include "layers/Resource/ProcessManager.hpp"
#include "layers/Resource/ResourceLayerFactory.hpp"
#include "layers/Resource/TimeManager.hpp"

#include <klib/klib.h>

EventRequestManager::EventRequestManager(ResourceLayerCaps* resources, LogicLayerCaps* logic) : Resources(resources), Logic(logic)
{
}

namespace
{
uint64_t check_event_signal_mask(uint64_t mask, uint64_t size)
{
    if (mask == 0)
        return 0;
    if (size != sizeof(uint64_t))
        return LINUX_EINVAL;
    uint64_t value;
    if (!safe_read_user(mask, &value, sizeof(value)))
        return LINUX_EFAULT;
    // Atomic signal-mask replacement needs the pending-signal subsystem.
    return LINUX_EOPNOTSUPP;
}
} // namespace

uint64_t EventRequestManager::WaitRequest(const arch_syscall_frame_t* frame, bool select, bool precise)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    if (Resources == nullptr || Resources->processManager == nullptr || Logic == nullptr || Logic->eventManager == nullptr)
        return LINUX_ENOSYS;
    if (precise)
    {
        uint64_t mask = frame->arg3, size = frame->arg4;
        if (select)
        {
            mask = size = 0;
            if (frame->arg5 != 0)
            {
                struct
                {
                    uint64_t mask;
                    uint64_t size;
                } bundle;
                if (!safe_read_user(frame->arg5, &bundle, sizeof(bundle)))
                    return LINUX_EFAULT;
                mask = bundle.mask;
                size = bundle.size;
            }
        }
        uint64_t result = check_event_signal_mask(mask, size);
        if (result != 0)
            return result;
    }
    uint32_t nfds;
    {
        FileDescriptorGuard guard(Resources->processManager->GetCurrentProcess());
        process_t*          process = Resources->processManager->GetCurrentProcess();
        if (process == nullptr)
            return LINUX_ESRCH;
        if (select)
        {
            const int32_t requested = (int32_t) frame->arg0;
            if (requested < 0)
                return LINUX_EINVAL;
            nfds = (uint32_t) ((uint64_t) requested < process->fileDescriptorCount ? (uint64_t) requested : process->fileDescriptorCount);
        }
        else
        {
            nfds = (uint32_t) frame->arg1;
            if (nfds > process->fileLimit.current)
                return LINUX_EINVAL;
        }
    }
    uint64_t        deadline       = UINT64_MAX;
    const uint64_t  timeoutAddress = select ? frame->arg4 : precise ? frame->arg2 : 0;
    time_timespec_t duration       = {};
    bool            finite         = timeoutAddress != 0;
    if (finite)
    {
        if (!safe_read_user(timeoutAddress, &duration, sizeof(duration)))
            return LINUX_EFAULT;
        if (select && !precise)
        {
            // Linux accepts non-normalized, nonnegative timeval microseconds.
            if (duration.seconds < 0 || duration.nanoseconds < 0)
                return LINUX_EINVAL;
            const int64_t extra = duration.nanoseconds / 1000000;
            if (duration.seconds > INT64_MAX - extra)
                return LINUX_EOVERFLOW;
            duration.seconds += extra;
            duration.nanoseconds = (duration.nanoseconds % 1000000) * 1000;
        }
        if (duration.seconds < 0 || duration.nanoseconds < 0 || duration.nanoseconds >= 1000000000)
            return LINUX_EINVAL;
    }
    else if (!select && !precise)
    {
        const int32_t milliseconds = (int32_t) frame->arg2;
        finite                     = milliseconds >= 0;
        if (finite)
        {
            duration.seconds     = milliseconds / 1000;
            duration.nanoseconds = (milliseconds % 1000) * 1000000LL;
        }
    }
    if (finite)
    {
        if (duration.seconds == 0 && duration.nanoseconds == 0)
            deadline = 0;
        else
        {
            if (Resources->timeManager == nullptr)
                return LINUX_ENOSYS;
            uint64_t result = Resources->timeManager->CalculateSleepDeadline(1, false, duration, &deadline);
            if (result != 0)
                return result;
            if (deadline == UINT64_MAX)
                return LINUX_EOVERFLOW;
        }
    }
    event_pollfd_t* descriptors = nullptr;
    uint64_t*       sets        = nullptr;
    const size_t    words       = ((size_t) nfds + 63) / 64;
    const size_t    setBytes    = words * sizeof(uint64_t);
    uint64_t        result      = [&]() -> uint64_t
    {
        if (!select)
        {
            if (nfds != 0)
            {
                descriptors = static_cast<event_pollfd_t*>(vmalloc((size_t) nfds * sizeof(event_pollfd_t)));
                if (descriptors == nullptr)
                    return LINUX_ENOMEM;
                if (!safe_read_user(frame->arg0, descriptors, (size_t) nfds * sizeof(event_pollfd_t)))
                    return LINUX_EFAULT;
            }
            uint64_t ready = Logic->eventManager->Poll(descriptors, nfds, deadline);
            if ((int64_t) ready < 0)
                return ready;
            for (uint32_t i = 0; i < nfds; ++i)
                if (!safe_write_user(frame->arg0 + (uint64_t) i * sizeof(event_pollfd_t) + offsetof(event_pollfd_t, revents), &descriptors[i].revents, sizeof(uint16_t)))
                    return LINUX_EFAULT;
            return ready;
        }
        const uint64_t userSets[3] = {frame->arg1, frame->arg2, frame->arg3};
        if (words != 0)
        {
            sets = static_cast<uint64_t*>(vmalloc(setBytes * 6));
            if (sets == nullptr)
                return LINUX_ENOMEM;
            memset(sets, 0, setBytes * 6);
            for (size_t set = 0; set < 3; ++set)
                if (userSets[set] != 0 && !safe_read_user(userSets[set], sets + set * words, setBytes))
                    return LINUX_EFAULT;
        }
        size_t count = 0;
        for (uint32_t fd = 0; fd < nfds; ++fd)
        {
            uint64_t bit = 1ULL << (fd % 64);
            if ((sets[fd / 64] | sets[words + fd / 64] | sets[words * 2 + fd / 64]) & bit)
                ++count;
        }
        if (count != 0)
        {
            descriptors = static_cast<event_pollfd_t*>(vmalloc(count * sizeof(event_pollfd_t)));
            if (descriptors == nullptr)
                return LINUX_ENOMEM;
        }
        size_t index = 0;
        for (uint32_t fd = 0; fd < nfds; ++fd)
        {
            const uint64_t bit    = 1ULL << (fd % 64);
            uint16_t       events = 0;
            if (sets[fd / 64] & bit)
                events |= EVENT_POLL_IN | EVENT_POLL_RDNORM | EVENT_POLL_RDBAND;
            if (sets[words + fd / 64] & bit)
                events |= EVENT_POLL_OUT | EVENT_POLL_WRNORM | EVENT_POLL_WRBAND;
            if (sets[words * 2 + fd / 64] & bit)
                events |= EVENT_POLL_PRI;
            if (events != 0)
                descriptors[index++] = {(int32_t) fd, events, 0};
        }
        uint64_t ready = Logic->eventManager->Poll(descriptors, count, deadline);
        if ((int64_t) ready < 0)
            return ready;
        uint64_t bitsReady = 0;
        for (size_t i = 0; i < count; ++i)
        {
            const event_pollfd_t& descriptor = descriptors[i];
            if (descriptor.revents & EVENT_POLL_NVAL)
                return LINUX_EBADF;
            const uint32_t fd       = (uint32_t) descriptor.fd;
            const uint64_t bit      = 1ULL << (fd % 64);
            const uint32_t masks[3] = {
                    EVENT_POLL_IN | EVENT_POLL_RDNORM | EVENT_POLL_RDBAND | EVENT_POLL_HUP | EVENT_POLL_ERR,
                    EVENT_POLL_OUT | EVENT_POLL_WRNORM | EVENT_POLL_WRBAND | EVENT_POLL_ERR,
                    EVENT_POLL_PRI,
            };
            for (size_t set = 0; set < 3; ++set)
                if ((sets[set * words + fd / 64] & bit) && (descriptor.revents & masks[set]))
                {
                    sets[(set + 3) * words + fd / 64] |= bit;
                    ++bitsReady;
                }
        }
        for (size_t set = 0; set < 3; ++set)
            if (userSets[set] != 0 && setBytes != 0 && !safe_write_user(userSets[set], sets + (set + 3) * words, setBytes))
                return LINUX_EFAULT;
        return bitsReady;
    }();
    vfree(descriptors);
    vfree(sets);
    if ((int64_t) result >= 0 && timeoutAddress != 0 && deadline != 0 && deadline != UINT64_MAX)
    {
        time_timespec_t now;
        if (Resources->timeManager != nullptr && Resources->timeManager->ReadClock(1, &now) == 0)
        {
            uint64_t        elapsed   = (uint64_t) now.seconds * 1000000000 + now.nanoseconds;
            uint64_t        left      = deadline > elapsed ? deadline - elapsed : 0;
            time_timespec_t remaining = {(int64_t) (left / 1000000000), (int64_t) (left % 1000000000)};
            if (select && !precise)
                remaining.nanoseconds /= 1000;
            // Like Linux, a read-only timeout must not turn successful I/O into EFAULT.
            (void) safe_write_user(timeoutAddress, &remaining, sizeof(remaining));
        }
    }
    return result;
}

uint64_t EventRequestManager::HandlePollRequest(const arch_syscall_frame_t* frame)
{
    return WaitRequest(frame, false, false);
}
uint64_t EventRequestManager::HandlePpollRequest(const arch_syscall_frame_t* frame)
{
    return WaitRequest(frame, false, true);
}
uint64_t EventRequestManager::HandleSelectRequest(const arch_syscall_frame_t* frame)
{
    return WaitRequest(frame, true, false);
}
uint64_t EventRequestManager::HandlePselect6Request(const arch_syscall_frame_t* frame)
{
    return WaitRequest(frame, true, true);
}
uint64_t EventRequestManager::HandleEventfdRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    if (Logic == nullptr || Logic->eventManager == nullptr)
        return LINUX_ENOSYS;
    return Logic->eventManager->CreateCounter((uint32_t) frame->arg0, 0);
}
uint64_t EventRequestManager::HandleEventfd2Request(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    if (Logic == nullptr || Logic->eventManager == nullptr)
        return LINUX_ENOSYS;
    return Logic->eventManager->CreateCounter((uint32_t) frame->arg0, (uint32_t) frame->arg1);
}

uint64_t EventRequestManager::HandleEpoll_createRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    if (Logic == nullptr || Logic->eventManager == nullptr)
        return LINUX_ENOSYS;
    if ((int32_t) frame->arg0 <= 0)
        return LINUX_EINVAL;
    return Logic->eventManager->CreateEpoll(0);
}

uint64_t EventRequestManager::HandleEpoll_create1Request(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    if (Logic == nullptr || Logic->eventManager == nullptr)
        return LINUX_ENOSYS;
    return Logic->eventManager->CreateEpoll((uint32_t) frame->arg0);
}

uint64_t EventRequestManager::HandleEpoll_ctlRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    if (Logic == nullptr || Logic->eventManager == nullptr)
        return LINUX_ENOSYS;
    const int32_t       operation = (int32_t) frame->arg1;
    event_epoll_event_t event;
    if (operation != 2 && !safe_read_user(frame->arg3, &event, sizeof(event)))
        return LINUX_EFAULT;
    return Logic->eventManager->ControlEpoll((int32_t) frame->arg0, operation, (int32_t) frame->arg2, operation == 2 ? nullptr : &event);
}

uint64_t EventRequestManager::HandleEpoll_waitRequest(const arch_syscall_frame_t* frame)
{
    if (frame == nullptr)
        return LINUX_EINVAL;
    if (Logic == nullptr || Logic->eventManager == nullptr)
        return LINUX_ENOSYS;
    const int32_t count = (int32_t) frame->arg2;
    if (count <= 0 || (uint32_t) count > INT32_MAX / sizeof(event_epoll_event_t))
        return LINUX_EINVAL;
    const size_t bytes = (size_t) count * sizeof(event_epoll_event_t);
    if (!range_within_u64(frame->arg1, bytes, CANONICAL_USER_BASE, CANONICAL_USER_END))
        return LINUX_EFAULT;
    uint64_t      deadline     = UINT64_MAX;
    const int32_t milliseconds = (int32_t) frame->arg3;
    if (milliseconds == 0)
        deadline = 0;
    else if (milliseconds > 0)
    {
        if (Resources == nullptr || Resources->timeManager == nullptr)
            return LINUX_ENOSYS;
        time_timespec_t duration = {milliseconds / 1000, (milliseconds % 1000) * 1000000LL};
        const uint64_t  status   = Resources->timeManager->CalculateSleepDeadline(1, false, duration, &deadline);
        if (status != 0)
            return status;
        if (deadline == UINT64_MAX)
            return LINUX_EOVERFLOW;
    }
    event_epoll_event_t* events = static_cast<event_epoll_event_t*>(vmalloc(bytes));
    if (events == nullptr)
        return LINUX_ENOMEM;
    uint64_t result = Logic->eventManager->WaitEpoll((int32_t) frame->arg0, events, (size_t) count, deadline);
    if ((int64_t) result > 0 && !safe_write_user(frame->arg1, events, (size_t) result * sizeof(event_epoll_event_t)))
        result = LINUX_EFAULT;
    vfree(events);
    return result;
}

uint64_t EventRequestManager::HandleEpoll_pwaitRequest(const arch_syscall_frame_t* frame)
{
    return frame == nullptr ? LINUX_EINVAL : LINUX_ENOSYS;
}

uint64_t EventRequestManager::HandleEpoll_pwait2Request(const arch_syscall_frame_t* frame)
{
    return frame == nullptr ? LINUX_EINVAL : LINUX_ENOSYS;
}

uint64_t EventRequestManager::HandleInotify_initRequest(const arch_syscall_frame_t* frame)
{
    return frame == nullptr ? LINUX_EINVAL : LINUX_ENOSYS;
}

uint64_t EventRequestManager::HandleInotify_init1Request(const arch_syscall_frame_t* frame)
{
    return frame == nullptr ? LINUX_EINVAL : LINUX_ENOSYS;
}

uint64_t EventRequestManager::HandleInotify_add_watchRequest(const arch_syscall_frame_t* frame)
{
    return frame == nullptr ? LINUX_EINVAL : LINUX_ENOSYS;
}

uint64_t EventRequestManager::HandleInotify_rm_watchRequest(const arch_syscall_frame_t* frame)
{
    return frame == nullptr ? LINUX_EINVAL : LINUX_ENOSYS;
}

uint64_t EventRequestManager::HandleSignalfdRequest(const arch_syscall_frame_t* frame)
{
    return frame == nullptr ? LINUX_EINVAL : LINUX_ENOSYS;
}

uint64_t EventRequestManager::HandleSignalfd4Request(const arch_syscall_frame_t* frame)
{
    return frame == nullptr ? LINUX_EINVAL : LINUX_ENOSYS;
}
