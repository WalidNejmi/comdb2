#ifndef INCLUDED_TRANLOG_FLAGS_H
#define INCLUDED_TRANLOG_FLAGS_H

enum {
    TRANLOG_FLAGS_BLOCK = 0x1,
    TRANLOG_FLAGS_DURABLE = 0x2,
    TRANLOG_FLAGS_DESCENDING = 0x4,
    TRANLOG_FLAGS_SENTINEL = 0x8,
};

enum { TRANLOG_CAP_TXN_COMMIT_FLAGS_V1 = 0x1 };

static inline int
tranlog_has_commit_flags_v1(unsigned int capabilities)
{
    return (capabilities & TRANLOG_CAP_TXN_COMMIT_FLAGS_V1) != 0;
}

static inline int
tranlog_reader_accepts_commit_flags(int commit_flags_capable,
                                    int record_has_commit_flags)
{
    return !record_has_commit_flags || commit_flags_capable;
}

#endif