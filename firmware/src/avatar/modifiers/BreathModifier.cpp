/**
 * BreathModifier — implementação
 */

#include "BreathModifier.h"
#include <math.h>

BreathModifier::BreathModifier(uint16_t cycleMs)
    : cycleMs_(cycleMs)
{
}

float BreathModifier::update(uint32_t nowMs) {
    if (cycleMs_ == 0) return 0.0f;
    float t = (float)(nowMs % cycleMs_) / (float)cycleMs_;
    float phase = 2.0f * PI * t;
    return (sinf(phase) + 1.0f) / 2.0f;
}
