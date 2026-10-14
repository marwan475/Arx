#pragma once
#include <klib/signal.h>

static_assert(sizeof(signal_action_t) == 32 && sizeof(signal_stack_t) == 24, "Linux LP64 signal ABI");
constexpr uint64_t SIGNAL_UNBLOCKABLE = (1ULL << 8) | (1ULL << 18);
constexpr uint64_t SIGNAL_DEFAULT_IGNORED = (1ULL << 16) | (1ULL << 22) | (1ULL << 27);
