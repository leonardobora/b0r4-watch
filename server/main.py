"""b0r4-watch server — entry point FastAPI."""

from contextlib import asynccontextmanager

from fastapi import FastAPI, WebSocket


@asynccontextmanager
async def lifespan(app: FastAPI):  # noqa: ARG001
    """Startup / shutdown hooks."""
    # TODO: iniciar clientes MQTT, LLM, etc.
    yield
    # TODO: fechar clientes


app = FastAPI(title="b0r4-watch server", lifespan=lifespan)


@app.get("/health")
async def health():
    """Health check usado pelo relógio para detectar o servidor."""
    return {"status": "ok", "project": "b0r4-watch"}


@app.websocket("/ws/audio")
async def audio_stream(websocket: WebSocket):
    """Stream de áudio do relógio para ASR -> LLM -> TTS.

    Fase 0: ecoa os bytes recebidos como confirmação de conexão.
    """
    await websocket.accept()
    try:
        while True:
            message = await websocket.receive_bytes()
            # Fase futura: encaminhar para ASR/LLM/TTS
            await websocket.send_text(f"received {len(message)} bytes")
    except Exception:
        # Cliente desconectou
        pass


if __name__ == "__main__":
    import uvicorn

    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)
