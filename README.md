# b0r4-watch

Smartwatch pessoal construído no **LILYGO T-Watch S3 (SX1262 915MHz)** com firmware ESP32 + servidor-agente Python.

## O que é

Duas pernas independentes:

- **`firmware/`** — Arduino + PlatformIO, LVGL, rodando no T-Watch S3.
- **`server/`** — Python (FastAPI + WebSocket + MQTT) para música (ytmusicapi), IA híbrida (ASR/LLM/TTS) e integrações.

## Decisões já tomadas

- **Hardware:** T-Watch S3, SX1262 915MHz
- **Toolchain:** Arduino + PlatformIO (lib oficial `t-watch-s3`)
- **IA:** híbrido 3 camadas — WakeNet9 (gatilho) + MultiNet (offline) + servidor cloud
- **Notificações:** ANCS via BLE do iPhone 13 (bots fora do escopo agora)
- **Casa:** Home Assistant + ESPHome (luzes na tela)
- **Energia:** BLE ativo + wake word sob demanda
- **Temas:** SquareLine Studio + gestos touch

Veja `docs/adr/` para decisões arquiteturais e `docs/CONTEXT.md` para o glossário.

## Estrutura

```
b0r4-watch/
├── firmware/     # PlatformIO T-Watch S3
├── server/       # Python FastAPI
└── docs/         # ADRs e glossário
```

## Milestones

1. **Setup (Fase 0)** — estrutura, compilar sem placa
2. **Bare metal** — flash, tela, RTC, sleep
3. **LVGL launcher + temas**
4. **ANCS (iPhone)**
5. **Música** (ytmusicapi + media keys + A2DP)
6. **Casa** (ESPHome/HA)
7. **IA híbrida**
8. **Hacker** (IR, LoRa, Wi-Fi scan)
9. **Futuro:** GPS shield, wake word always-on, TLS

## Requisitos

- Python 3.12+
- PlatformIO Core (`pip install platformio`)
- VS Code + extensão PlatformIO IDE (recomendado)

## Como começar

### Firmware

```bash
cd firmware
pio run
```

### Servidor

```bash
cd server
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
copy env.example .env
uvicorn main:app --reload
```

## Licença

MIT
