#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BUFFER_DECL(op, size) \
    const uint8_t* buffer_##op##_##size(uint8_t*, const uint8_t*, const uint8_t*, size_t)
#define BUFFER_SIZES(op) \
    BUFFER_DECL(op, 16); BUFFER_DECL(op, 64); BUFFER_DECL(op, 256)
BUFFER_DECL(Put, 4);
BUFFER_DECL(Put, 8);
BUFFER_SIZES(Put);
BUFFER_DECL(Get, 4);
BUFFER_DECL(Get, 8);
BUFFER_SIZES(Get);
BUFFER_SIZES(NoNetPut);
BUFFER_SIZES(NoNetGet);
#undef BUFFER_SIZES
#undef BUFFER_DECL

#ifdef __cplusplus
}
#endif
