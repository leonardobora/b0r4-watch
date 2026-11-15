/*
 * mesh_node.h — the two pieces of mesh state that are pure data: the node table
 * (who I have heard, and how well) and the mailbox (recent text messages).
 *
 * No Arduino, no radio, no LVGL: this is the part that gets host tests, because
 * "which node is that and did the message survive a wrap" is exactly the logic
 * that is impossible to watch on a 240px screen.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "mesh_defs.h"

namespace mesh {

constexpr size_t kMaxNodes   = 32;
constexpr size_t kLongNameN  = 40;   // Meshtastic long names are ~39 usable chars
constexpr size_t kShortNameN = 5;    // 4 chars + NUL
constexpr size_t kMaxMsgs    = 16;
constexpr size_t kMsgTextN   = 180;

struct Node {
    bool     used = false;
    uint32_t num = 0;
    char     longName[kLongNameN] = {0};
    char     shortName[kShortNameN] = {0};
    uint8_t  hwModel = 0;
    uint8_t  role = 0;
    int8_t   snr = 0;          // dB (rounded from the float on the wire)
    int8_t   hops = 0;         // hops away, 0 = heard directly
    uint32_t lastHeardS = 0;   // device uptime seconds, not wall clock
    bool     self = false;
};

struct Msg {
    bool     used = false;
    uint32_t id = 0;
    uint32_t from = 0;         // 0 == us
    uint32_t to = 0;
    uint32_t atS = 0;          // device uptime seconds
    int8_t   snr = 0;
    uint8_t  hops = 0;
    bool     outgoing = false;
    char     text[kMsgTextN] = {0};
};

// Fixed-capacity table with oldest-heard eviction. A mesh can have hundreds of
// nodes and this watch has no business holding them all; it holds the ones it
// has actually heard from recently.
class NodeDb {
public:
    void   setSelf(uint32_t num, const char *longName, const char *shortName);
    uint32_t selfNum() const { return _selfNum; }

    // Insert or refresh. `longName`/`shortName` may be null to leave them as
    // they are (a NodeInfo we have not received yet, or a position packet).
    Node  *upsert(uint32_t num, const char *longName = nullptr,
                  const char *shortName = nullptr, uint32_t nowS = 0);
    Node  *find(uint32_t num);
    const Node *find(uint32_t num) const;

    // What a UI/serial dump wants: nodes by recency, self first.
    size_t count() const;
    size_t snapshot(Node *out, size_t max) const;   // returns how many were written
    Node  *at(size_t i);                            // raw slot access for iteration

    void   heard(uint32_t num, int8_t snr, uint8_t hops, uint32_t nowS);

    // A display name that is never empty: short name, else long name, else hex id.
    static void displayName(const Node &n, char *out, size_t outN);
    static void hexId(uint32_t num, char *out, size_t outN);   // "!a1b2c3d4"

private:
    Node     _nodes[kMaxNodes];
    uint32_t _selfNum = 0;
};

// Ring buffer of the most recent messages, newest last.
class Mailbox {
public:
    // Returns false when the text does not fit (it is stored, not truncated:
    // a shortened message that looks complete is a lie about what was sent).
    bool push(const char *text, uint32_t from, uint32_t to, uint32_t id,
              int8_t snr, uint8_t hops, bool outgoing, uint32_t nowS);

    size_t size() const { return _count; }
    // i = 0 is the OLDEST held message.
    const Msg *at(size_t i) const;
    const Msg *newest() const;

    // Everything newer than `sinceId`, oldest first. Returns the count written.
    size_t since(uint32_t sinceId, Msg *out, size_t max) const;

    uint32_t lastId() const { return _lastId; }
    void clear() { _count = 0; _next = 0; _lastId = 0; }

private:
    Msg      _msgs[kMaxMsgs];
    size_t   _count = 0;
    size_t   _next = 0;      // write cursor (oldest slot when full)
    uint32_t _lastId = 0;
};

// Copy with a guaranteed NUL and no read past the source. Returns the length
// copied (excluding the NUL).
size_t copyStr(char *dst, size_t dstN, const char *src, size_t srcN);

}  // namespace mesh
