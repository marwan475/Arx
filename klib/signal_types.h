#ifndef KLIB_SIGNAL_TYPES_H
#define KLIB_SIGNAL_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef struct signal_action_t
{
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
} signal_action_t;

typedef struct signal_stack_t
{
    uint64_t sp;
    int32_t  flags;
    uint32_t padding;
    uint64_t size;
} signal_stack_t;

typedef struct signal_sender_t
{
    int32_t  pid;
    uint32_t uid;
    int32_t  code;
    uint64_t value;
} signal_sender_t;

static const uint64_t SIGNAL_UNBLOCKABLE     = (UINT64_C(1) << 8) | (UINT64_C(1) << 18);
static const uint64_t SIGNAL_DEFAULT_IGNORED = (UINT64_C(1) << 16) | (UINT64_C(1) << 17) | (UINT64_C(1) << 22) | (UINT64_C(1) << 27);

#ifdef __cplusplus
static_assert(sizeof(signal_action_t) == 32 && offsetof(signal_action_t, mask) == 24, "Linux LP64 signal action ABI");
static_assert(sizeof(signal_stack_t) == 24 && offsetof(signal_stack_t, size) == 16, "Linux LP64 signal stack ABI");
static_assert(sizeof(signal_sender_t) == 24 && offsetof(signal_sender_t, value) == 16, "signal sender storage layout");
#endif

#endif
