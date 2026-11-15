/*
 * mesh_defs.h — the handful of constants both the wire layer and the state
 * layer need. Split out so the two headers can be included together without
 * redefining anything.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace mesh {

// Meshtastic's broadcast node number (NODENUM_BROADCAST).
constexpr uint32_t kBroadcast = 0xFFFFFFFFUL;

// Semtech's own ceiling for an SX12xx LoRa frame, which is also the firmware's
// MAX_LORA_PAYLOAD_LEN: the 16-byte packet header comes out of the same budget.
constexpr size_t kHeaderLen  = 16;
constexpr size_t kMaxFrame   = 255;
constexpr size_t kMaxPayload = kMaxFrame - kHeaderLen;   // 239

}  // namespace mesh
