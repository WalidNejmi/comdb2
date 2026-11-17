#include "buffer_adapter.h"
#include <endian_core.h>

/* Keep the production C inline helpers in C; amortize the language boundary. */
#define BUFFER_IMPL(op, size, expression) \
    const uint8_t* buffer_##op##_##size(uint8_t* dst, const uint8_t* src, \
                                       const uint8_t* end, size_t count) \
    { \
        const uint8_t* result = NULL; \
        for (size_t i = 0; i < count; ++i) { \
            result = (expression); \
            __asm__ __volatile__("" : : "r"(result) : "memory"); \
        } \
        return result; \
    }
#define BUFFER_PAIR(size) \
    BUFFER_IMPL(Put, size, buf_put(src, size, dst, end)) \
    BUFFER_IMPL(Get, size, buf_get(dst, size, src, end))
#define BUFFER_NO_NET_PAIR(size) \
    BUFFER_IMPL(NoNetPut, size, buf_no_net_put(src, size, dst, end)) \
    BUFFER_IMPL(NoNetGet, size, buf_no_net_get(dst, size, src, end))
BUFFER_PAIR(4)
BUFFER_PAIR(8)
BUFFER_PAIR(16)
BUFFER_PAIR(64)
BUFFER_PAIR(256)
BUFFER_NO_NET_PAIR(16)
BUFFER_NO_NET_PAIR(64)
BUFFER_NO_NET_PAIR(256)
