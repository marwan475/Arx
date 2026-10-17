#define _GNU_SOURCE
#include <features.h>
#include <unistd.h>

#ifdef __GLIBC__
#error "Dynamic ELF fixtures require musl"
#endif

extern int DynamicLibraryProbe(void);

int main(void)
{
    if (DynamicLibraryProbe() != 42)
    {
        return 1;
    }

    return syscall(0x1338, 42) == 0 ? 0 : 2;
}
