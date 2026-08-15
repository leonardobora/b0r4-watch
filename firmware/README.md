# b0r4-watch — firmware

Firmware Arduino + PlatformIO para o **LILYGO T-Watch S3 (SX1262 915MHz)**.

## Requisitos

- [PlatformIO Core](https://platformio.org/install) (`pip install platformio`)
- VS Code + extensão PlatformIO IDE (opcional, mas recomendado)

## Compilar

```bash
cd firmware
pio run
```

A primeira vez baixa todas as libs (~500MB) e compila o ESP32 Arduino core.

## Estrutura

```
firmware/
├── boards/                    # definição da placa LilyGoWatch-S3
├── include/                   # Setup212 do TFT_eSPI (vendored)
├── src/
│   └── main.cpp               # entry point mínimo
└── platformio.ini             # config do projeto
```

## Upload

Com o relógio conectado via USB e em modo download (segure BOOT, aperte RST, solte BOOT):

```bash
pio run --target upload
pio device monitor
```

## Recuperação

Se o sketch travar e impedir upload, coloque o relógio em modo download manualmente. O botão BOOT fica dentro do relógio; veja:
https://github.com/Xinyuan-LilyGO/TTGO_TWatch_Library/tree/t-watch-s3/firmware#note
