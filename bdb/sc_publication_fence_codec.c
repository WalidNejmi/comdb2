#include "sc_publication_fence_codec.h"

#include <string.h>

#include <endian_core.h>

int sc_publication_fence_decode(const uint8_t *key, size_t keylen,
                                const uint8_t *value, size_t valuelen,
                                SC_PUBLICATION_FENCE_RECORD *record)
{
    const uint8_t *cursor = value, *end;
    int version, lsn_file, lsn_offset, i;
    uint64_t old_build_id;

    if (key == NULL || value == NULL || record == NULL ||
        keylen != SC_PUBLICATION_FENCE_KEY_LEN)
        return -1;
    end = value + valuelen;

    cursor = buf_get(&version, sizeof(version), cursor, end);
    cursor = buf_get(&lsn_file, sizeof(lsn_file), cursor, end);
    cursor = buf_get(&lsn_offset, sizeof(lsn_offset), cursor, end);
    if (cursor == NULL)
        return -1;

    memset(&record->build_id, 0, sizeof(record->build_id));
    record->publication_utxnid = 0;
    if (valuelen == SC_PUBLICATION_FENCE_DATA_V1_LEN && version == 1) {
        cursor = buf_get(&old_build_id, sizeof(old_build_id), cursor, end);
        if (cursor == NULL || old_build_id == 0)
            return -1;
        memset(&record->build_id, 0xff, sizeof(record->build_id));
        memcpy(record->build_id.bytes, &old_build_id, sizeof(old_build_id));
    } else if (valuelen == SC_PUBLICATION_FENCE_DATA_V2_LEN && version == 2) {
        cursor = buf_no_net_get(record->build_id.bytes,
                                sizeof(record->build_id.bytes), cursor, end);
        if (cursor == NULL || sc_build_id_is_zero(&record->build_id))
            return -1;
    } else if (valuelen == SC_PUBLICATION_FENCE_DATA_LEN &&
               version == SC_PUBLICATION_FENCE_VERSION) {
        cursor = buf_no_net_get(record->build_id.bytes,
                                sizeof(record->build_id.bytes), cursor, end);
        cursor = buf_get(&record->publication_utxnid,
                         sizeof(record->publication_utxnid), cursor, end);
        if (cursor == NULL || sc_build_id_is_zero(&record->build_id) ||
            record->publication_utxnid == 0)
            return -1;
    } else {
        return -1;
    }

    if (cursor != end || lsn_file <= 0 || lsn_offset < 0)
        return -1;

    for (i = 0; i < DB_FILE_ID_LEN; i++)
        if (key[4 + i] != 0)
            break;
    if (i == DB_FILE_ID_LEN)
        return -1;

    memcpy(record->fileid, key + 4, DB_FILE_ID_LEN);
    record->fence_lsn.file = (unsigned int)lsn_file;
    record->fence_lsn.offset = (unsigned int)lsn_offset;
    return 0;
}