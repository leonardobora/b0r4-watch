/**
 * b0r4-watch Avatar — implementação
 */

#include "Avatar.h"
#include <stdio.h>
#include <math.h>

Avatar::Avatar(lv_obj_t* parent) {
    label_ = lv_label_create(parent);
    lv_obj_set_style_text_align(label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label_, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(label_, " o     o\n   \\_/");
}

void Avatar::update(const FaceState& state) {
    // Respiração move o rosto verticalmente entre -2 e +2 px (sutil, mas contínua).
    int breathOffset = (int)roundf((state.breath - 0.5f) * 4.0f);
    lv_obj_set_y(label_, breathOffset);

    // Só re-renderiza a string quando algo visível mudou (economia de CPU).
    bool changed = (state.emotion != lastEmotion_) ||
                   (state.blink > 0.5f) != (lastBlink_ > 0.5f) ||
                   (state.breath > 0.5f) != (lastBreath_ > 0.5f);
    if (!changed) return;

    char buf[96];
    render(state, buf, sizeof(buf));
    lv_label_set_text(label_, buf);

    lastEmotion_ = state.emotion;
    lastBlink_ = state.blink;
    lastBreath_ = state.breath;
}

void Avatar::render(const FaceState& state, char* buf, size_t len) {
    char eye = eyeChar(state);
    const char* mouth = mouthString(state);
    const char* name = emotionName(state);

    snprintf(buf, len, " %c     %c\n   %s\n  %s",
             eye, eye, mouth, name);
}

char Avatar::eyeChar(const FaceState& state) const {
    if (state.blink > 0.5f) {
        return '-';
    }
    switch (state.emotion) {
        case Emotion::HAPPY:     return '^';
        case Emotion::ANGRY:     return '>';
        case Emotion::SAD:       return 'T';
        case Emotion::SURPRISED: return 'O';
        case Emotion::SLEEPY:    return '-';
        case Emotion::DOUBT:     return 'o';
        case Emotion::NEUTRAL:
        default:                 return 'o';
    }
}

const char* Avatar::mouthString(const FaceState& state) const {
    switch (state.emotion) {
        case Emotion::HAPPY:     return "\\ _ /";   // \ _ /
        case Emotion::ANGRY:     return "\\___/";   // \___/
        case Emotion::SAD:       return "/   \\";   // /   \
        case Emotion::SURPRISED: return "  O  ";
        case Emotion::SLEEPY:    return "  o  ";
        case Emotion::DOUBT:     return " \\?/ ";
        case Emotion::NEUTRAL:
        default:                 return " \\_/ ";
    }
}

const char* Avatar::emotionName(const FaceState& state) const {
    switch (state.emotion) {
        case Emotion::HAPPY:     return "happy";
        case Emotion::ANGRY:     return "angry";
        case Emotion::SAD:       return "sad";
        case Emotion::SURPRISED: return "wow";
        case Emotion::SLEEPY:    return "sleepy";
        case Emotion::DOUBT:     return "hmm";
        case Emotion::NEUTRAL:
        default:                 return "b0r4";
    }
}
