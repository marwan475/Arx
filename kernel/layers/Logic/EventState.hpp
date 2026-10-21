#pragma once

#include "EventManager.hpp"
#include "VirtualFileSystem.hpp"

// Borrowed event state. Reader bounds and identity contracts: EventDebugStats.hpp.
struct event_epoll_watch_t
{
    file_close_observer_t observer;
    event_epoll_t*        owner;
    file_t*               file; // Weak: its intrusive close observer detaches before release/free.
    int32_t               descriptor;
    event_epoll_event_t   event;
    bool                  armed;
    bool                  pending;
    event_epoll_watch_t*  next;
};

struct event_epoll_t
{
    file_close_observer_t observer;
    uint64_t              sequence;
    inode_t               inode;
    EventManager*         manager;
    file_t*               file;
    event_epoll_watch_t*  head;
    event_epoll_watch_t*  tail;
    event_epoll_t*        next;
    uint64_t              topologyGeneration;
    size_t                topologyDepth;
    uint64_t              notificationGeneration;
};

struct event_counter_t
{
    inode_t       inode;
    spinlock_t    lock;
    uint64_t      value;
    bool          semaphore;
    EventManager* manager;
};

struct notification_record_t
{
    int32_t  descriptor;
    uint32_t mask;
    uint32_t cookie;
    uint32_t length;
    char     name[256];
};

static_assert(offsetof(notification_record_t, name) == 16, "Linux inotify_event header");

struct notification_watch_t
{
    inode_t* inode;
    uint32_t mask;
    int32_t  descriptor;
};

struct event_notification_t
{
    inode_t               inode;
    EventManager*         manager;
    file_t*               file;
    event_notification_t* next;
    uint32_t              kind;
};

struct inotify_state_t
{
    event_notification_t  common;
    notification_watch_t  watches[128];
    notification_record_t records[64];
    size_t                head;
    size_t                count;
    int32_t               nextDescriptor;
};

struct signal_descriptor_t
{
    event_notification_t common;
    uint64_t             mask;
};

