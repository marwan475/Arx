#pragma once
#include <stdint.h>

typedef struct signal_action_t
{
    uint64_t handler, flags, restorer, mask;
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
} signal_sender_t;
