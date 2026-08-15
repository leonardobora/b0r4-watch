/**
 * BlinkModifier
 *
 * Portado do StackChan-Gotchi:
 * - olhos abertos por ~5,2 s + jitter aleatório de 0..500 ms
 * - olhos fechados por ~200 ms
 */

#ifndef B0R4_BLINK_MODIFIER_H
#define B0R4_BLINK_MODIFIER_H

#include <Arduino.h>

class BlinkModifier {
public:
    BlinkModifier(uint16_t openBaseMs = 5200, uint16_t closeMs = 200);

    // Retorna 0.0f (aberto) ou 1.0f (fechado)
    float update(uint32_t nowMs);

private:
    uint16_t openBaseMs_;
    uint16_t closeMs_;
    uint16_t currentOpenMs_;
    uint32_t lastToggleMs_;
    bool closed_;
};

#endif // B0R4_BLINK_MODIFIER_H
