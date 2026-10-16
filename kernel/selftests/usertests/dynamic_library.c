static int           initialized;
static int           counter[1024] __attribute__((aligned(4096))) = {40};
static unsigned char zeroes[8192];

__attribute__((constructor)) static void Initialize(void)
{
    initialized = 1;
}

int DynamicLibraryProbe(void)
{
    if (initialized != 1 || counter[0] != 40 || zeroes[0] != 0 || zeroes[8191] != 0)
    {
        return -1;
    }

    zeroes[8191] = 1;
    counter[0] += 2;
    return counter[0];
}
