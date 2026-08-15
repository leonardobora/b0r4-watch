/**
 * BlinkModifier — implementação
 */

#include "BlinkModifier.h"

BlinkModifier::BlinkModifier(uint16_t openBaseMs, uint16_t closeMs)
    : openBaseMs_(openBaseMs)
    , closeMs_(closeMs)
    , currentOpenMs_(openBaseMs)
    , lastToggleMs_(0)
    , closed_(false)
{
}

float BlinkModifier::update(uint32_t nowMs) {
    uint16_t interval = closed_ ? closeMs_ : currentOpenMs_;
    if (nowMs - lastToggleMs_ >= interval) {
        closed_ = !closed_;
        lastToggleMs_ = nowMs;
        if (!closed_) {
            // Próxima abertura dura um pouco diferente (jitter biológico).
            currentOpenMs_ = openBaseMs_ + random(0, 500);
        }
    }
    return closed_ ? 1.0f : 0.0f;
}
