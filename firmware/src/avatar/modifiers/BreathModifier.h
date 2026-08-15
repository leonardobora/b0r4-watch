/**
 * BreathModifier
 *
 * Portado do StackChan-Gotchi:
 * - onda senoidal suave usada para movimento vertical de respiração
 * - ciclo padrão de 6 s
 */

#ifndef B0R4_BREATH_MODIFIER_H
#define B0R4_BREATH_MODIFIER_H

#include <Arduino.h>

class BreathModifier {
public:
    explicit BreathModifier(uint16_t cycleMs = 6000);

    // Retorna 0.0f..1.0f ao longo de um ciclo senoidal
    float update(uint32_t nowMs);

private:
    uint16_t cycleMs_;
};

#endif // B0R4_BREATH_MODIFIER_H
