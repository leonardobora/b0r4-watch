#include "mesh_node.h"

#include <stdio.h>
#include <string.h>

namespace mesh {

size_t copyStr(char *dst, size_t dstN, const char *src, size_t srcN)
{
    if (!dst || dstN == 0) return 0;
    size_t n = 0;
    if (src) {
        while (n < srcN && n + 1 < dstN && src[n] != '\0') n++;
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
    return n;
}

void NodeDb::hexId(uint32_t num, char *out, size_t outN)
{
    snprintf(out, outN, "!%08lx", (unsigned long)num);
}

void NodeDb::displayName(const Node &n, char *out, size_t outN)
{
    if (n.shortName[0]) { snprintf(out, outN, "%s", n.shortName); return; }
    if (n.longName[0])  { snprintf(out, outN, "%s", n.longName);  return; }
    hexId(n.num, out, outN);
}

void NodeDb::setSelf(uint32_t num, const char *longName, const char *shortName)
{
    _selfNum = num;
    Node *n = upsert(num, longName, shortName, 0);
    if (n) n->self = true;
}

// Full table: evict the node we have not heard from for the longest, but never
// us. If every slot is fresh and every one of them is "now", the oldest is still
// the right one to drop — anything else would mean refusing to learn a new node.
Node *NodeDb::upsert(uint32_t num, const char *longName, const char *shortName, uint32_t nowS)
{
    Node *free_slot = nullptr;
    size_t oldest = 0;
    bool have_oldest = false;

    for (size_t i = 0; i < kMaxNodes; i++) {
        Node &n = _nodes[i];
        if (!n.used) { free_slot = &n; break; }
        if (n.num == num) {
            if (longName)  copyStr(n.longName,  sizeof(n.longName),  longName,  kLongNameN);
            if (shortName) copyStr(n.shortName, sizeof(n.shortName), shortName, kShortNameN);
            return &n;
        }
        if (!n.self && (!have_oldest || n.lastHeardS < _nodes[oldest].lastHeardS)) {
            oldest = i;
            have_oldest = true;
        }
    }

    if (!free_slot) {
        if (!have_oldest) return nullptr;      // every slot is self: cannot happen
        free_slot = &_nodes[oldest];
        *free_slot = Node{};
        free_slot->used = true;
        free_slot->num = num;
    } else {
        free_slot->used = true;
        free_slot->num = num;
    }

    if (longName)  copyStr(free_slot->longName,  sizeof(free_slot->longName),  longName,  kLongNameN);
    if (shortName) copyStr(free_slot->shortName, sizeof(free_slot->shortName), shortName, kShortNameN);
    free_slot->lastHeardS = nowS;
    return free_slot;
}

Node *NodeDb::find(uint32_t num)
{
    for (size_t i = 0; i < kMaxNodes; i++)
        if (_nodes[i].used && _nodes[i].num == num) return &_nodes[i];
    return nullptr;
}

const Node *NodeDb::find(uint32_t num) const
{
    return const_cast<NodeDb *>(this)->find(num);
}

void NodeDb::heard(uint32_t num, int8_t snr, uint8_t hops, uint32_t nowS)
{
    Node *n = upsert(num, nullptr, nullptr, nowS);
    if (!n) return;
    n->snr = snr;
    n->hops = hops;
    n->lastHeardS = nowS;
}

size_t NodeDb::count() const
{
    size_t c = 0;
    for (size_t i = 0; i < kMaxNodes; i++) if (_nodes[i].used) c++;
    return c;
}

Node *NodeDb::at(size_t i)
{
    return (i < kMaxNodes && _nodes[i].used) ? &_nodes[i] : nullptr;
}

// Self first, then most recently heard. Insertion sort over at most 32 entries:
// one bubble pass per output slot, no allocation, predictable cost.
size_t NodeDb::snapshot(Node *out, size_t max) const
{
    size_t have = 0;
    Node tmp[kMaxNodes];
    for (size_t i = 0; i < kMaxNodes; i++) if (_nodes[i].used) tmp[have++] = _nodes[i];

    for (size_t i = 1; i < have; i++) {
        Node key = tmp[i];
        size_t j = i;
        while (j > 0 && (tmp[j - 1].lastHeardS < key.lastHeardS)) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = key;
    }

    size_t n = 0;
    for (size_t i = 0; i < have; i++) {
        if (tmp[i].self) continue;           // self is reported separately
        if (n < max) out[n++] = tmp[i];
    }
    return n;
}

// ------------------------------------------------------------------- mailbox
bool Mailbox::push(const char *text, uint32_t from, uint32_t to, uint32_t id,
                   int8_t snr, uint8_t hops, bool outgoing, uint32_t nowS)
{
    if (!text) return false;
    // Refuse rather than truncate: the caller must know the text did not fit.
    if (strlen(text) >= kMsgTextN) return false;

    Msg &m = _msgs[_next];
    m = Msg{};
    m.used = true;
    m.id = id;
    m.from = outgoing ? 0 : from;
    m.to = to;
    m.atS = nowS;
    m.snr = snr;
    m.hops = hops;
    m.outgoing = outgoing;
    copyStr(m.text, sizeof(m.text), text, kMsgTextN - 1);

    _next = (_next + 1) % kMaxMsgs;
    if (_count < kMaxMsgs) _count++;
    if (id) _lastId = id;
    return true;
}

const Msg *Mailbox::at(size_t i) const
{
    if (i >= _count) return nullptr;
    // When the ring has wrapped, slot 0 is the oldest.
    const size_t start = (_count == kMaxMsgs) ? _next : 0;
    return &_msgs[(start + i) % kMaxMsgs];
}

const Msg *Mailbox::newest() const
{
    return _count ? at(_count - 1) : nullptr;
}

size_t Mailbox::since(uint32_t sinceId, Msg *out, size_t max) const
{
    size_t n = 0;
    for (size_t i = 0; i < _count && n < max; i++) {
        const Msg *m = at(i);
        if (!m) continue;
        if (sinceId == 0 || m->id > sinceId) out[n++] = *m;
    }
    return n;
}

}  // namespace mesh
