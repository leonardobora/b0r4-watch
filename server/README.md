# b0r4-watch — server

Servidor-agente Python para o relógio. Responsabilidades:

- Receber stream de áudio do relógio via WebSocket (`/ws/audio`).
- Orquestrar ASR → LLM → TTS para comandos de linguagem livre.
- Controlar YouTube Music via `ytmusicapi`.
- Disparar media keys no PC Windows.
- (Futuro) Integrar com Home Assistant quando necessário.

## Requisitos

- Python 3.11+

## Instalação

```bash
cd server
python -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
copy env.example .env
# edite .env com suas chaves
```

## Rodar

```bash
uvicorn main:app --reload
```

O endpoint `/health` fica disponível em http://localhost:8000/health.
