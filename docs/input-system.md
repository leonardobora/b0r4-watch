# Sistema de Input do b0r4-watch

## Visão

O relógio é controlado por uma combinação intencional de inputs:

- **Gestos físicos** (acelerômetro BMA423)
- **Botão POWER** (PMU AXP2101)
- **Touchscreen capacitivo**
- **Voz** (WakeNet/MultiNet — documentado em outro ADR)

O princípio é **economia + intenção**: sem always-on, ações disparadas por gestos claros e configuráveis.

## Componentes

### Gesture Engine

Módulo central que escuta eventos de input e os traduz em **Actions**.

```cpp
struct Gesture {
    String id;
    GestureSource source;  // ACCEL, BUTTON, TOUCH, VOICE
    String pattern;
};

struct Action {
    String id;
    ActionType type;  // LOCAL, INTENT, SERVER
    String payload;
};
```

### Fontes de input

| Fonte | Sensor/Botão | Eventos disponíveis |
|---|---|---|
| `ACCEL` | BMA423 | tap, double_tap, wrist_tilt, any_motion, no_motion, custom_shake |
| `BUTTON` | POWER (AXP2101) | single_click, double_click, long_press |
| `TOUCH` | FT6X36 | tap, long_tap, swipe_up, swipe_down, swipe_left, swipe_right |
| `VOICE` | PDM mic + ESP-SR | wake_word, offline_intent |

### Gestos customizáveis

O mapa gesto → ação pode ser carregado de `gestures.json` no FATFS:

```json
{
  "gestures": [
    {
      "id": "double_shake_side",
      "source": "ACCEL",
      "pattern": "shake_x:2",
      "action": "assistant.listen"
    },
    {
      "id": "power_double_click",
      "source": "BUTTON",
      "pattern": "double_click",
      "action": "music.next"
    },
    {
      "id": "wrist_tilt",
      "source": "ACCEL",
      "pattern": "wrist_tilt",
      "action": "screen.wake"
    }
  ]
}
```

## Detecção de "chacoalhar pro lado"

Implementação customizada no eixo X do acelerômetro:

1. Amostrar BMA423 a ~50 Hz.
2. Detectar cruzamento de threshold positivo/negativo no eixo X.
3. Contar um "shake" como par positivo+negativo.
4. Se 2 shakes em menos de ~800 ms, dispara o gesto.

```cpp
void updateShakeDetector() {
    float x = watch.readBMA().x;
    uint32_t now = millis();

    int sign = (x > 0) - (x < 0);

    if (sign != 0 && sign != lastSign && abs(x) > SHAKE_THRESHOLD) {
        shakeCount++;
        lastSign = sign;
        lastShakeTime = now;
        if (shakeCount == 1) firstShakeTime = now;
    }

    if (shakeCount >= 4 && (now - firstShakeTime) < SHAKE_WINDOW_MS) {
        trigger("double_shake_side");
        resetShake();
    }

    if ((now - lastShakeTime) > SHAKE_TIMEOUT_MS) {
        resetShake();
    }
}
```

> `shakeCount >= 4` porque cada "chacoalhada pro lado" gera ~2 picos (ida e volta).

## Mapeamento sugerido de fábrica

| Gesto | Ação padrão | Contexto |
|---|---|---|
| Levantar pulso | `screen.wake` | Sempre ativo, baixo consumo |
| Chacoalhar 2x pro lado | `assistant.listen` | Tela apagada ou ligada |
| Duplo clique POWER | `music.next` | Qualquer estado |
| Segurar POWER 1 s | `lights.toggle` | Tela ligada |
| Segurar POWER 3 s | `system.power_menu` | Qualquer estado |
| Tap na tela | `ui.select` | No launcher |
| Swipe left/right | `ui.prev_card` / `ui.next_card` | No watchface |
| Swipe up | `launcher.open` | Do watchface |
| Swipe down | `settings.quick_panel` | Do watchface |

## Considerações de energia

- O BMA423 suporta wake-up por motion em hardware, consumindo microamperes.
- O firmware mantém o BMA423 em modo de baixa energia e só acorda o ESP32 quando um gesto válido é detectado.
- Shake detection customizado exige o ESP32 ligeiramente mais ativo que os gestos nativos. Recomenda-se taxa de amostragem baixa (~50 Hz) e janela curta.

## Extensibilidade para white-label

Para shipar versões customizadas para clientes:

1. Criar `profiles/<cliente>/gestures.json`.
2. No build, copiar o profile desejado para `data/gestures.json` (partição FATFS).
3. O `GestureEngine` carrega o profile em runtime.

Assim cada cliente pode ter seus próprios atalhos e branding sem forkar o firmware.
