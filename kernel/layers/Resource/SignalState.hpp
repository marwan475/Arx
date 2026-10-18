#pragma once

#include <klib/signal_types.h>

struct signal_queue_t
{
    static constexpr size_t CAPACITY = 128;

    struct record_t
    {
        uint32_t        number;
        signal_sender_t sender;
    };

    size_t   count;
    size_t   reserved;
    record_t records[CAPACITY];
};
