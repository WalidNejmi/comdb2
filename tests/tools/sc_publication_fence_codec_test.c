#include <stdio.h>
#include <string.h>

#include <endian_core.h>
#include <flibc.h>

#include "sc_publication_fence_codec.h"

static int failures;

#define CHECK(condition, label)                                                \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(stderr, "FAIL: %s\n", label);                            \
            failures++;                                                        \
        }                                                                      \
    } while (0)

static void encode_key(uint8_t *key)
{
    int type = 60;
    int i;

    memset(key, 0, SC_PUBLICATION_FENCE_KEY_LEN);
    buf_put(&type, sizeof(type), key, key + SC_PUBLICATION_FENCE_KEY_LEN);
    for (i = 0; i < DB_FILE_ID_LEN; i++)
        key[4 + i] = (uint8_t)(i + 1);
}

static size_t encode_value(uint8_t *value, int version, int lsn_file,
                           int lsn_offset, int v1)
{
    uint8_t *cursor = value;
    uint64_t old_build_id = 0x1122334455667788ULL;
    sc_build_id_t build_id;
    int i;

    for (i = 0; i < SC_BUILD_ID_LEN; i++)
        build_id.bytes[i] = (uint8_t)(0xa0 + i);
    cursor = buf_put(&version, sizeof(version), cursor,
                     value + SC_PUBLICATION_FENCE_DATA_LEN);
    cursor = buf_put(&lsn_file, sizeof(lsn_file), cursor,
                     value + SC_PUBLICATION_FENCE_DATA_LEN);
    cursor = buf_put(&lsn_offset, sizeof(lsn_offset), cursor,
                     value + SC_PUBLICATION_FENCE_DATA_LEN);
      if (v1) {
            uint64_t network_build_id = flibc_htonll(old_build_id);
            cursor = buf_no_net_put(&network_build_id, sizeof(network_build_id),
                                                cursor, value + SC_PUBLICATION_FENCE_DATA_LEN);
      } else {
        cursor = buf_no_net_put(build_id.bytes, sizeof(build_id.bytes), cursor,
                                value + SC_PUBLICATION_FENCE_DATA_LEN);
            if (version == SC_PUBLICATION_FENCE_VERSION) {
                  uint64_t publication_utxnid = 0x8877665544332211ULL;
                  uint64_t network_utxnid = flibc_htonll(publication_utxnid);
                  cursor = buf_no_net_put(&network_utxnid, sizeof(network_utxnid),
                                                      cursor, value + SC_PUBLICATION_FENCE_DATA_LEN);
            }
      }
    return (size_t)(cursor - value);
}

int main(void)
{
    uint8_t key[SC_PUBLICATION_FENCE_KEY_LEN];
    uint8_t value[SC_PUBLICATION_FENCE_DATA_LEN + 1];
    SC_PUBLICATION_FENCE_RECORD record;
    size_t len;

    encode_key(key);
    len = encode_value(value, 2, 7, 99, 0);
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len, &record) == 0,
          "valid v2 rejected");
    len = encode_value(value, 3, 7, 99, 0);
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len, &record) == 0,
          "valid v3 rejected");
    len = encode_value(value, 1, 7, 99, 1);
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len, &record) == 0,
          "valid v1 rejected");

            len = encode_value(value, 3, 7, 99, 0);
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, 0, &record) != 0,
          "zero-length value accepted");
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len - 1, &record) != 0,
          "short value accepted");
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len + 1, &record) != 0,
          "oversized value accepted");
    CHECK(sc_publication_fence_decode(key, sizeof(key) - 1, value, len, &record) != 0,
          "short key accepted");

    len = encode_value(value, 99, 7, 99, 0);
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len, &record) != 0,
          "unknown version accepted");
            len = encode_value(value, 3, 0, 99, 0);
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len, &record) != 0,
          "zero LSN file accepted");
            len = encode_value(value, 3, 7, -1, 0);
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len, &record) != 0,
          "negative LSN offset accepted");

    len = encode_value(value, 3, 7, 99, 0);
    memset(value + 12, 0, SC_BUILD_ID_LEN);
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len, &record) != 0,
          "zero build id accepted");
    len = encode_value(value, 3, 7, 99, 0);
    memset(value + 12 + SC_BUILD_ID_LEN, 0, sizeof(uint64_t));
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len, &record) != 0,
          "zero publication utxnid accepted");
    len = encode_value(value, 3, 7, 99, 0);
    memset(key + 4, 0, DB_FILE_ID_LEN);
    CHECK(sc_publication_fence_decode(key, sizeof(key), value, len, &record) != 0,
          "zero file id accepted");

    printf("sc_publication_fence_codec_test: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}