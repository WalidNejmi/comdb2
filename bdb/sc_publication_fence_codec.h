#ifndef INCLUDED_SC_PUBLICATION_FENCE_CODEC_H
#define INCLUDED_SC_PUBLICATION_FENCE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include <build/db.h>

enum {
    SC_PUBLICATION_FENCE_KEY_LEN = 4 + DB_FILE_ID_LEN,
    SC_PUBLICATION_FENCE_DATA_V1_LEN = 4 + 4 + 4 + 8,
    SC_PUBLICATION_FENCE_DATA_V2_LEN = 4 + 4 + 4 + SC_BUILD_ID_LEN,
    SC_PUBLICATION_FENCE_DATA_LEN = 4 + 4 + 4 + SC_BUILD_ID_LEN + 8,
    SC_PUBLICATION_FENCE_VERSION = 3
};

int sc_publication_fence_decode(const uint8_t *key, size_t keylen,
                                const uint8_t *value, size_t valuelen,
                                SC_PUBLICATION_FENCE_RECORD *record);

#endif