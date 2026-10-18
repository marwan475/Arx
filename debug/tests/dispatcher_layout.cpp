#include <kernel/layers/Dispatcher.hpp>

#if !DEBUG
static_assert(sizeof(Dispatcher) == 3 * sizeof(void*), "release Dispatcher contains debug storage");
#endif

char arx_dispatcher_layout_probe[sizeof(Dispatcher)];
