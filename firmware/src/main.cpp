/**
 * b0r4-watch firmware — Fase 0
 *
 * Mínimo compilável para o LILYGO T-Watch S3 (SX1262).
 * Quando a placa chegar, este sketch acende a tela com um "hello".
 */

#include <LilyGoLib.h>
#include <LV_Helper.h>
#include "avatar/Avatar.h"
#include "avatar/modifiers/BlinkModifier.h"
#include "avatar/modifiers/BreathModifier.h"

#define USE_RADIO_SX1262

#if defined(USE_RADIO_SX1262)
SX1262 radio = newModule();
#endif

// Avatar e modifiers (alocação dinâmica porque precisam de LVGL já iniciado)
static Avatar* avatar = nullptr;
static BlinkModifier* blink = nullptr;
static BreathModifier* breath = nullptr;

static const Emotion kDemoEmotions[] = {
    Emotion::NEUTRAL,
    Emotion::HAPPY,
    Emotion::ANGRY,
    Emotion::SAD,
    Emotion::SURPRISED,
    Emotion::SLEEPY,
    Emotion::DOUBT
};
static constexpr size_t kDemoEmotionCount = sizeof(kDemoEmotions) / sizeof(kDemoEmotions[0]);
static size_t currentEmotionIndex = 0;

static void avatar_update_cb(lv_timer_t*)
{
    if (!avatar || !blink || !breath) return;

    uint32_t now = millis();
    FaceState state;
    state.emotion = kDemoEmotions[currentEmotionIndex];
    state.blink = blink->update(now);
    state.breath = breath->update(now);
    state.gazeX = 0;
    state.gazeY = 0;

    avatar->update(state);
}

static void emotion_cycle_cb(lv_timer_t*)
{
    currentEmotionIndex = (currentEmotionIndex + 1) % kDemoEmotionCount;
}

void setup()
{
    Serial.begin(115200);

    watch.begin();
    beginLvglHelper();

    avatar = new Avatar(lv_scr_act());
    blink = new BlinkModifier();
    breath = new BreathModifier();

    // Atualiza face a ~30 FPS; emoção muda a cada 3 s.
    lv_timer_create(avatar_update_cb, 33, nullptr);
    lv_timer_create(emotion_cycle_cb, 3000, nullptr);
}

void loop()
{
    lv_timer_handler();
    delay(5);
}
