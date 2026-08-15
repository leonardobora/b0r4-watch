/**
 * b0r4-watch Avatar
 *
 * Renderiza um rosto ASCII em um label LVGL, com estado de emoção,
 * piscar e respiração. Inspirado no StackChan.
 */

#ifndef B0R4_AVATAR_H
#define B0R4_AVATAR_H

#include <lvgl.h>

enum class Emotion {
    NEUTRAL,
    HAPPY,
    ANGRY,
    SAD,
    SURPRISED,
    SLEEPY,
    DOUBT
};

struct FaceState {
    Emotion emotion = Emotion::NEUTRAL;
    float blink = 0.0f;       // 0 = olhos abertos, 1 = fechados
    float breath = 0.0f;      // 0..1, usado para movimento vertical suave
    int8_t gazeX = 0;         // -2..2 (reservado)
    int8_t gazeY = 0;         // -2..2 (reservado)
};

class Avatar {
public:
    explicit Avatar(lv_obj_t* parent);

    void update(const FaceState& state);
    lv_obj_t* label() const { return label_; }

private:
    lv_obj_t* label_;
    Emotion lastEmotion_ = Emotion::NEUTRAL;
    float lastBlink_ = -1.0f;
    float lastBreath_ = -1.0f;

    void render(const FaceState& state, char* buf, size_t len);
    char eyeChar(const FaceState& state) const;
    const char* mouthString(const FaceState& state) const;
    const char* emotionName(const FaceState& state) const;
};

#endif // B0R4_AVATAR_H
