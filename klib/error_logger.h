#ifndef KLIB_ERROR_LOGGER_H
#define KLIB_ERROR_LOGGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ERROR_LOGGER_CAPACITY 64
#define ERROR_LOGGER_MESSAGE_SIZE 256

// Zero-initialize before use. Published entries are immutable; do not reset a
// buffer while writers/readers can access it. No heap or clock is required.
typedef struct error_log_entry
{
    uint8_t ready;
    char message[ERROR_LOGGER_MESSAGE_SIZE];
} error_log_entry_t;

typedef struct error_log_buffer
{
    uint64_t reserved;
    error_log_entry_t entries[ERROR_LOGGER_CAPACITY];
} error_log_buffer_t;

#ifdef __cplusplus
extern "C" {
#endif

// Record a kernel diagnostic in the global boot buffer and write it to serial.
// This is explicit logging, not automatic capture of syscall errno returns.
void error_logger(const char* format, ...);
// Snapshot to KDEBUG (terminal when DEBUG=1) and serial.
void error_logger_print(void);
// Instance operations allow consumers/tests to use an isolated fixed buffer.
bool error_logger_append(error_log_buffer_t* buffer, const char* format, ...);
bool error_logger_read(const error_log_buffer_t* buffer, size_t index, char* message, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
