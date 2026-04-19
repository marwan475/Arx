#ifndef KLIB_BITMAP_H
#define KLIB_BITMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Convert a bit count to the minimum number of bytes needed.
#define BITMAP_BYTES_FOR_BITS(bit_count) (((bit_count) + 7u) / 8u)

// Return the byte index for a bit.
#define BITMAP_BYTE_INDEX(bit_index) ((bit_index) / 8u)

// Return the bit mask inside a byte for a bit.
#define BITMAP_BIT_MASK(bit_index) ((uint8_t)(1u << ((bit_index) & 7u)))

static inline void bitmap_init(uint8_t* bitmap, size_t bit_count)
{
    size_t byte_count = BITMAP_BYTES_FOR_BITS(bit_count);

    for (size_t i = 0; i < byte_count; i++)
    {
        bitmap[i] = 0;
    }
}

static inline void bitmap_set(uint8_t* bitmap, size_t bit_index)
{
    bitmap[BITMAP_BYTE_INDEX(bit_index)] |= BITMAP_BIT_MASK(bit_index);
}

static inline void bitmap_clear(uint8_t* bitmap, size_t bit_index)
{
    bitmap[BITMAP_BYTE_INDEX(bit_index)] &= (uint8_t)~BITMAP_BIT_MASK(bit_index);
}

static inline bool bitmap_test(const uint8_t* bitmap, size_t bit_index)
{
    return (bitmap[BITMAP_BYTE_INDEX(bit_index)] & BITMAP_BIT_MASK(bit_index)) != 0;
}

static inline void bitmap_set_range(uint8_t* bitmap, size_t first_bit, size_t bit_count)
{
    for (size_t i = 0; i < bit_count; i++)
    {
        bitmap_set(bitmap, first_bit + i);
    }
}

static inline void bitmap_clear_range(uint8_t* bitmap, size_t first_bit, size_t bit_count)
{
    for (size_t i = 0; i < bit_count; i++)
    {
        bitmap_clear(bitmap, first_bit + i);
    }
}

// Find first clear bit in [0, bit_count). Returns true and stores index on success.
static inline bool bitmap_find_first_clear(const uint8_t* bitmap, size_t bit_count, size_t* out_index)
{
    for (size_t i = 0; i < bit_count; i++)
    {
        if (!bitmap_test(bitmap, i))
        {
            if (out_index != NULL)
            {
                *out_index = i;
            }
            return true;
        }
    }

    return false;
}

// Find first set bit in [0, bit_count). Returns true and stores index on success.
static inline bool bitmap_find_first_set(const uint8_t* bitmap, size_t bit_count, size_t* out_index)
{
    for (size_t i = 0; i < bit_count; i++)
    {
        if (bitmap_test(bitmap, i))
        {
            if (out_index != NULL)
            {
                *out_index = i;
            }
            return true;
        }
    }

    return false;
}

#endif