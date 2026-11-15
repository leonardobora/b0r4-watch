#include "proto.h"

#include <string.h>

namespace mesh {

void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

uint32_t get32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// --------------------------------------------------------------------- writer
bool Writer::varint(uint64_t v)
{
    // 10 bytes is the most a 64-bit varint can take.
    uint8_t tmp[10];
    size_t k = 0;
    do {
        uint8_t byte = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v) byte |= 0x80;
        tmp[k++] = byte;
    } while (v);

    if (bad || n + k > cap) { bad = true; return false; }
    memcpy(b + n, tmp, k);
    n += k;
    return true;
}

bool Writer::tag(uint32_t field, uint32_t wire)
{
    return varint(((uint64_t)field << 3) | (uint64_t)wire);
}

bool Writer::putVarintField(uint32_t field, uint64_t v)
{
    if (!tag(field, 0)) return false;
    return varint(v);
}

bool Writer::putBytes(uint32_t field, const uint8_t *p, size_t len)
{
    if (!tag(field, 2) || !varint(len)) return false;
    if (bad || n + len > cap) { bad = true; return false; }
    memcpy(b + n, p, len);
    n += len;
    return true;
}

bool Writer::putString(uint32_t field, const char *s)
{
    return putBytes(field, (const uint8_t *)s, strlen(s));
}

bool Writer::putFixed32Field(uint32_t field, uint32_t v)
{
    if (!tag(field, 5)) return false;
    if (bad || n + 4 > cap) { bad = true; return false; }
    put32le(b + n, v);
    n += 4;
    return true;
}

// --------------------------------------------------------------------- reader
bool nextField(const uint8_t *d, size_t len, size_t &off, Field &f)
{
    if (off >= len) return false;

    uint64_t key = 0;
    int shift = 0;
    while (true) {
        if (off >= len || shift > 63) return false;
        const uint8_t byte = d[off++];
        key |= ((uint64_t)(byte & 0x7f)) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }

    f = Field{};
    f.num = (uint32_t)(key >> 3);
    f.wire = (uint32_t)(key & 7);
    if (f.num == 0) return false;   // field 0 does not exist

    switch (f.wire) {
        case 0: {
            uint64_t v = 0;
            shift = 0;
            while (true) {
                if (off >= len || shift > 63) return false;
                const uint8_t byte = d[off++];
                v |= ((uint64_t)(byte & 0x7f)) << shift;
                if (!(byte & 0x80)) break;
                shift += 7;
            }
            f.v = v;
            return true;
        }
        case 1:
            if (off + 8 > len) return false;
            f.v = (uint64_t)get32le(d + off) | ((uint64_t)get32le(d + off + 4) << 32);
            off += 8;
            return true;
        case 2: {
            uint64_t l = 0;
            shift = 0;
            while (true) {
                if (off >= len || shift > 63) return false;
                const uint8_t byte = d[off++];
                l |= ((uint64_t)(byte & 0x7f)) << shift;
                if (!(byte & 0x80)) break;
                shift += 7;
            }
            if (off + l > len) return false;
            f.bytes = d + off;
            f.len = (size_t)l;
            off += (size_t)l;
            return true;
        }
        case 5:
            if (off + 4 > len) return false;
            f.v = get32le(d + off);
            off += 4;
            return true;
        default:
            return false;   // groups (3/4) are not in any Meshtastic message we use
    }
}

bool findVarint(const uint8_t *d, size_t len, uint32_t want, uint64_t &out)
{
    size_t off = 0;
    Field f;
    while (nextField(d, len, off, f)) {
        if (f.num == want && f.wire == 0) { out = f.v; return true; }
    }
    return false;
}

bool findBytes(const uint8_t *d, size_t len, uint32_t want, const uint8_t *&ptr, size_t &blen)
{
    size_t off = 0;
    Field f;
    while (nextField(d, len, off, f)) {
        if (f.num == want && f.wire == 2) { ptr = f.bytes; blen = f.len; return true; }
    }
    return false;
}

}  // namespace mesh
