/**
 * b0r4-watch firmware — Fase 0
 *
 * Mínimo compilável para o LILYGO T-Watch S3 (SX1262).
 * Quando a placa chegar, este sketch acende a tela com um "hello".
 */

#include <LilyGoLib.h>
#include <LV_Helper.h>

#define USE_RADIO_SX1262

#if defined(USE_RADIO_SX1262)
SX1262 radio = newModule();
#endif

void setup()
{
    Serial.begin(115200);

    watch.begin();
    beginLvglHelper();

    // Tela mínima de boas-vindas
    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_label_set_text(label, "b0r4-watch\nhello");
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
}

void loop()
{
    lv_timer_handler();
    delay(5);
}
