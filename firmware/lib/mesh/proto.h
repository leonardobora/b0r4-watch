/*
 * proto.h — the smallest protobuf writer/reader that speaks the Meshtastic
 * messages this firmware needs (Data, User, Position, NodeInfo, Routing).
 *
 * Deliberately hand-rolled and dependency-free: the same code must build for
 * the ESP32 and for the host tests, and the wire format is the interoperability
 * contract with real Meshtastic nodes — so it is exactly the part that must be
 * testable without hardware.
 *
 * Wire types used by those messages, and nothing else:
 *   0 varint, 1 fixed64, 2 length-delimited, 5 fixed32.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mesh {

// --------------------------------------------------------------------- writer
class Writer {
public:
    Writer(uint8_t *buf, size_t cap) : b(buf), cap(cap) {}

    size_t size() const { return n; }
    // False the moment a write did not fit. Callers MUST check: a silently
    // truncated protobuf is a packet that other nodes reject for no visible
    // reason, which is the worst kind of bug to chase over the air.
    bool ok() const { return !bad; }
    const uint8_t *data() const { return b; }

    bool varint(uint64_t v);
    bool tag(uint32_t field, uint32_t wire);
    bool putVarintField(uint32_t field, uint64_t v);
    bool putBytes(uint32_t field, const uint8_t *p, size_t len);
    bool putString(uint32_t field, const char *s);
    bool putFixed32Field(uint32_t field, uint32_t v);   // float and sfixed32 both
    bool putBool(uint32_t field, bool v) { return putVarintField(field, v ? 1 : 0); }

private:
    uint8_t *b;
    size_t   cap;
    size_t   n = 0;
    bool     bad = false;
};

// --------------------------------------------------------------------- reader
struct Field {
    uint32_t       num = 0;
    uint32_t       wire = 0;
    uint64_t       v = 0;        // varint / fixed32 / fixed64 value
    const uint8_t *bytes = nullptr;   // length-delimited payload
    size_t         len = 0;
};

// Reads one tagged field. Returns false at the end of the buffer or on a
// malformed one — an over-the-air packet is untrusted input.
bool nextField(const uint8_t *d, size_t len, size_t &off, Field &f);

// Convenience scans: find a field by number, ignoring unknown fields (which is
// what lets a newer Meshtastic firmware talk to us without breaking).
bool findVarint(const uint8_t *d, size_t len, uint32_t want, uint64_t &out);
bool findBytes(const uint8_t *d, size_t len, uint32_t want, const uint8_t *&ptr, size_t &blen);

// Little-endian helpers, named so the wire order is never in doubt.
void put32le(uint8_t *p, uint32_t v);
uint32_t get32le(const uint8_t *p);

}  // namespace mesh
