# ADR 0001: Toolchain do firmware — Arduino + PlatformIO

## Contexto
Precisávamos escolher entre Arduino+PlatformIO, ESP-IDF nativo e MicroPython para o firmware do T-Watch S3.

## Decisão
Usar **Arduino + PlatformIO** com a biblioteca oficial `Xinyuan-LilyGO/TTGO_TWatch_Library` branch `t-watch-s3`.

## Consequências
- Drivers de display, touch, PMU, acelerômetro, RTC, haptic, áudio e LoRa já estão integrados.
- Iteração rápida; exemplos cobrem todos os periféricos.
- Menor controle de energia do que ESP-IDF puro.
- ESP-IDF permanece como opção de migração futura se o gerenciamento de energia virar gargalo.
