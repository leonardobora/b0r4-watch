# ADR 0008: Sistema de Avatar para o b0r4-watch

## Contexto

O relógio precisa de uma presença visual mínima: algo que indique que ele está "vivo" em idle, que reaja a eventos (notificações, comando de voz, erro de rede) e que seja leve o suficiente para rodar em uma tela 240×240 com 8 MB de PSRAM.

Inspirado no StackChan (M5Stack CoreS3), queremos um rosto expressivo e animações idle (piscar, respirar), mas adaptado ao formato de pulso e à nossa toolchain Arduino + PlatformIO + LVGL.

## Decisão

1. **Renderização via LVGL `lv_label`** usando arte ASCII com fonte monoespaçada (ideal `LV_FONT_UNSCII_8`/`16`).
   - Mais leve que shapes vetoriais ou bitmaps.
   - Fácil de trocar em tempo de execução (mudar string = mudar expressão).
   - Funciona como protótipo imediato; no futuro pode evoluir para shapes LVGL ou sprites.

2. **Modelo de estado `FaceState`** com:
   - `Emotion` (`NEUTRAL`, `HAPPY`, `ANGRY`, `SAD`, `SURPRISED`, `SLEEPY`, `DOUBT`)
   - `blink` (0..1, vem do `BlinkModifier`)
   - `breath` (0..1, vem do `BreathModifier`)
   - `gazeX`, `gazeY` (reservado para movimentar pupilas no futuro)

3. **Modifiers independentes** que rodam no `loop()` / `lv_timer`:
   - `BlinkModifier`: pisca a cada ~5,2 s, pálpebra fecha em ~200 ms (portado do StackChan-Gotchi).
   - `BreathModifier`: onda senoidal de ~6 s que modifica levemente a posição vertical do rosto (portado do StackChan-Gotchi).

4. **Demo integrada ao `main.cpp`** que cicla emoções a cada 3 s para validar o sistema enquanto a placa não chega.

## Consequências

- Economia de recursos: zero imagens, zero spritesheets, RAM usada apenas para o label LVGL.
- A arte ASCII é limitada em refinamento; aceitamos isso na Fase 0 e migramos para shapes/sprites só se necessário.
- O mesmo `FaceState` pode ser enviado pelo servidor Python no futuro, permitindo que o assistente remoto controle a expressão do relógio via WebSocket.
- Módulo desacoplado: `Avatar` + `Modifiers` podem ser reutilizados em outras telas (launcher, notificações, chamada do assistente).

## Alternativas consideradas

- **Shapes LVGL** (círculos + retângulos): usado pelo StackChan-Gotchi, é o próximo passo natural, mas exige mais código de desenho por frame.
- **Sprites bitmap**: descartado por consumo de flash e dificuldade de adicionar novas expressões.
- **Canvas com draw APIs**: flexível, mas mais RAM/CPU que um label ASCII.

## Links de referência

- [`stack-chan/stack-chan`](https://github.com/stack-chan/stack-chan) — firmware original TypeScript/Moddable/Piu
- [`weirdglitch-42/StackChan-Gotchi`](https://github.com/weirdglitch-42/StackChan-Gotchi) — port ESP-IDF + LVGL com sistema de modifiers
- [`c1pher-cn/ha-mcp-for-xiaozhi`](https://github.com/c1pher-cn/ha-mcp-for-xiaozhi) — referência de integração HA/MCP

## Status

Aceito — implementado na Fase 0 como protótipo.
