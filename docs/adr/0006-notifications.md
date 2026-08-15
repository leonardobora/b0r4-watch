# ADR 0006: Notificações — somente ANCS via BLE

## Contexto
Há interesse em notificações de agentes/bots, mas também em notificações nativas do iPhone.

## Decisão
No escopo inicial, notificações vêm **apenas via ANCS (Apple Notification Center Service) sobre BLE** do iPhone 13. Bots e agentes próprios ficam fora do escopo de notificações por enquanto.

## Consequências
- Funciona nativamente com WhatsApp, ligações, etc.
- O servidor Python fica focado em música e IA, não em ser hub de notificações.
- Notificações de agentes podem ser adicionadas futuramente via MQTT/WebSocket.
