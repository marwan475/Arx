#include <arch/arch.h>

typedef struct discriptor
{
    union
    {
        uint64_t value;
        struct
        {
            uint64_t limit_15_0 : 16;
            uint64_t base_15_0 : 16;
            uint64_t base_23_16 : 8;
            uint64_t type : 4;
            uint64_t s : 1;
            uint64_t dpl : 2;
            uint64_t p : 1;
            uint64_t limit_19_16 : 4;
            uint64_t avl : 1;
            uint64_t l : 1;
            uint64_t d_b : 1;
            uint64_t g : 1;
            uint64_t base_31_24 : 8;
        };
    };
} discriptor_t;