# ADR 0005: Política de energia — BLE ativo, wake word sob demanda

## Contexto
T-Watch S3 tem bateria ~400–580mAh. Wi-Fi consome 88–283mA; BLE fica em µA–mA.

## Decisão
- BLE sempre ligado para ANCS (notificações do iPhone).
- Wi-Fi ativado sob demanda (música, IA, configuração).
- Wake word sob demanda (não sempre ouvindo).
- Deep sleep entre eventos, com wake por touch, BMA423, RTC ou botão Power.

## Consequências
- Dias de autonomia com alertas em tempo real.
- Sessões de voz/IA exigem ativação manual ou por gesto.
- Wake word always-on pode ser ativado futuramente como modo premium de bateria.
