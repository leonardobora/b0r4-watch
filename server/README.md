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

## Pipeline de voz

O endpoint `/ws/audio` recebe chunks de áudio PCM 16kHz mono 16-bit e devolve:

1. Transcrição do usuário (`ASR`)
2. Resposta do assistente (`LLM`)
3. Resultado de tool calls (música, luzes, etc.)
4. Áudio da resposta (`TTS`)

### Protocolo WebSocket

- `{"type": "start"}` — inicia gravação
- binary frames — chunks de áudio
- `{"type": "end"}` — finaliza e processa
- Server responde com JSON (`text`, `tool_call`, `audio`) e bytes de áudio.

### Providers configuráveis

| Etapa | Providers | Padrão |
|---|---|---|
| ASR | `openai` (Whisper), `mock` | `mock` |
| LLM | `openai` (GPT), `mock` | `mock` |
| TTS | `openai`, `edge_tts` (gratuito), `mock` | `mock` |

Para usar OpenAI, preencha `OPENAI_API_KEY` no `.env` e mude `ASR_PROVIDER=openai`, `LLM_PROVIDER=openai`, etc.

Para TTS gratuito com `edge_tts`, apenas defina `TTS_PROVIDER=edge_tts` (não precisa de API key).

## Testar sem o relógio

```bash
# terminal 1
uvicorn main:app --reload

# terminal 2
python scripts/test_client.py
```

O cliente envia 1 segundo de silêncio e mostra a resposta completa do pipeline.
