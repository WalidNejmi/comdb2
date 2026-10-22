/*
   Copyright 2015 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include "comdb2uuid.h"
#ifdef __linux__
#include <sys/random.h>
#endif

/*
 * Random (version 4) uuid, the same kind uuid_generate() returns. libuuid
 * opens, reads and closes /dev/urandom and reseeds rand() on every call;
 * one getrandom() call gives the same bytes much more cheaply.
 */
void comdb2uuid(uuid_t u)
{
#ifdef __linux__
    if (getrandom(u, sizeof(uuid_t), 0) == sizeof(uuid_t)) {
        u[6] = (u[6] & 0x0f) | 0x40; /* version 4 */
        u[8] = (u[8] & 0x3f) | 0x80; /* RFC 4122 variant */
        return;
    }
#endif
    uuid_generate(u);
}

char *comdb2uuidstr(uuid_t u, char out[37]);
inline char *comdb2uuidstr(uuid_t u, char out[37])
{
    uuid_unparse(u, out);
    return out;
}

void comdb2uuid_clear(uuid_t u) { uuid_clear(u); }

int comdb2uuidcmp(uuid_t u1, uuid_t u2) { return uuid_compare(u1, u2); }

void comdb2uuidcpy(uuid_t dst, uuid_t src) { uuid_copy(dst, src); }

int comdb2uuid_is_zero(uuid_t u)
{
    uuid_t zero;
    comdb2uuid_clear(zero);
    return !comdb2uuidcmp(u, zero);
}
