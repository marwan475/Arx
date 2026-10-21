#pragma once
#include "CommunicationDebug.hpp"

#if DEBUG
class EventManager;
struct SignalDebugStats;

// arx.events/1. source's Epolls and Notifications are borrowed intrusive roots.
// Notifications has at most 64 nodes; epoll/watch chains have no fixed capacity:
// use explicit reader entry/byte/scan budgets and cycle detection. Nesting <=5.
// EventState.hpp defines layouts. Verify exact operations, privateData, inode and
// manager identities before casting. Epoll watch files are WEAK: validate close
// observer membership and stop on a retiring file; never retain or Poll it.
// armed/pending are stored hints, never computed readiness or delivery history.
// Generation is the general wait sequence; TopologyGeneration invalidates depth
// caches; NotificationGeneration deduplicates propagation; ExclusiveGeneration
// rotates exclusive selection. All are wrapping counters, not object identities.
// Notification kind 1=inotify, 2=signalfd. Inotify has 128 physical watch slots
// (null inode=unused), and a 64-record FIFO: physical=(head+i)%64, count<=64,
// head<64. length<=256, with bounded NUL/padding. Overflow REPLACES the newest
// record; derive it from queued IN_OVERFLOW, not a persistent flag. Coalesced
// duplicates append nothing. queued/overflow/coalesced count enqueue outcomes;
// inotifyCopied/signalfdCopied count complete records copied to user memory.
// signalfd mask matches the caller's process|task pending union; descriptor
// ownership correlations are contexts, not an intrinsic signal recipient.
// Event counters are descriptor-discovered, absent from Notifications. live*
// includes initialized objects whose descriptor installation may later fail.
// Source/VFS/signal/general notifications count attempts, not delivered events.
// pendingTransitions counts false->true transitions in NotifySourceLocked only.
// counterReads/counterWrites count successful eight-byte state transfers, not
// byte totals or user call attempts. copyFailures covers epoll/notification
// record copies; failed copies may already have changed part of user memory.
// epollCopied counts records copied by ScanEpoll, never readiness-only scans.
// Source locks and provider groups can be interrupted: report partial, not atomic
// snapshots. All counters are manager-lifetime modular uint64_t. Release erases
// the stats and every instrumentation expression.
struct EventDebugStats
{
    EventManager* source;
    const ProcessDebugStats* processes;
    const SignalDebugStats* signals;
    const void* counterOperations;
    const void* epollOperations;
    const void* inotifyOperations;
    const void* signalOperations;
    const void* watchClosing;
    const void* epollClosing;
    const void* readinessLock;
    arx_debug_group_t group;
    CommunicationOperationStats createCounter;
    CommunicationOperationStats createEpoll;
    CommunicationOperationStats controlEpoll;
    CommunicationOperationStats waitEpoll;
    CommunicationOperationStats createInotify;
    CommunicationOperationStats addWatch;
    CommunicationOperationStats removeWatch;
    CommunicationOperationStats signalDescriptor;
    uint64_t liveCounters;
    uint64_t liveEpolls;
    uint64_t liveNotifications;
    uint64_t notifications;
    uint64_t sourceNotifications;
    uint64_t vfsNotifications;
    uint64_t signalNotifications;
    uint64_t pendingTransitions;
    uint64_t epollCopied;
    uint64_t counterReads;
    uint64_t counterWrites;
    uint64_t inotifyQueued;
    uint64_t inotifyCoalesced;
    uint64_t inotifyOverflow;
    uint64_t inotifyCopied;
    uint64_t signalfdCopied;
    uint64_t copyFailures;
};
#endif
