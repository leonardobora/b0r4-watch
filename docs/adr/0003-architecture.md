# ADR 0003: Arquitetura — duas pernas (servidor + cliente relógio)

## Contexto
O projeto poderia ser um monólito centrado no relógio ou uma plataforma com servidor reutilizável.

## Decisão
Criar duas pernas independentes:
- `firmware/` rodando no T-Watch S3.
- `server/` Python (FastAPI + WebSocket + MQTT) servindo música e IA.

## Consequências
- O servidor pode ser reutilizado por outros clientes futuros.
- Integração com Home Assistant fica via ESPHome, não através do servidor.
- Mais clareza de responsabilidades, mas dois artefatos para manter.
