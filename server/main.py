"""b0r4-watch server — entry point FastAPI."""

import json
from contextlib import asynccontextmanager

import uvicorn
from fastapi import FastAPI, WebSocket, WebSocketDisconnect

from src.asr import get_asr_provider
from src.audio import AudioBuffer
from src.config import settings
from src.llm import get_llm_provider
from src.tts import get_tts_provider


@asynccontextmanager
async def lifespan(app: FastAPI):  # noqa: ARG001
    app.state.asr = get_asr_provider()
    app.state.llm = get_llm_provider()
    app.state.tts = get_tts_provider()
    yield


app = FastAPI(title="b0r4-watch server", lifespan=lifespan)


@app.get("/health")
async def health():
    """Health check usado pelo relógio para detectar o servidor."""
    return {
        "status": "ok",
        "project": "b0r4-watch",
        "asr_provider": settings.asr_provider,
        "llm_provider": settings.llm_provider,
        "tts_provider": settings.tts_provider,
    }


@app.websocket("/ws/audio")
async def audio_stream(websocket: WebSocket):
    """Stream de áudio do relógio para ASR -> LLM -> TTS.

    Protocolo:
    - JSON {"type": "start"}  -> inicia gravação
    - binary frames           -> chunks de áudio PCM 16kHz mono 16-bit
    - JSON {"type": "end"}    -> finaliza e processa
    - Server responde com JSON (text, tool_call, audio) e bytes de áudio.
    """
    await websocket.accept()
    buffer = AudioBuffer()

    try:
        while True:
            message = await websocket.receive()

            if "text" in message:
                try:
                    data = json.loads(message["text"])
                except json.JSONDecodeError:
                    await websocket.send_json(
                        {"type": "error", "message": "Invalid JSON"}
                    )
                    continue

                msg_type = data.get("type")

                if msg_type == "start":
                    buffer.reset()
                    await websocket.send_json(
                        {"type": "status", "message": "listening"}
                    )

                elif msg_type == "end":
                    await websocket.send_json(
                        {"type": "status", "message": "processing"}
                    )
                    audio_bytes = buffer.to_wav_bytes()

                    transcription = await websocket.app.state.asr.transcribe(
                        audio_bytes, language=settings.assistant_language
                    )
                    await websocket.send_json(
                        {"type": "text", "role": "user", "content": transcription}
                    )

                    result = await websocket.app.state.llm.chat(
                        transcription, language=settings.assistant_language
                    )
                    assistant_text = result["text"]

                    await websocket.send_json(
                        {
                            "type": "text",
                            "role": "assistant",
                            "content": assistant_text,
                        }
                    )

                    for tc in result.get("tool_calls", []):
                        await websocket.send_json({"type": "tool_call", "data": tc})

                    if assistant_text:
                        audio_response = await websocket.app.state.tts.synthesize(
                            assistant_text, language=settings.assistant_language
                        )
                        audio_format = "wav" if settings.tts_provider == "mock" else "mp3"
                        await websocket.send_json(
                            {"type": "audio", "format": audio_format}
                        )
                        await websocket.send_bytes(audio_response)

                    buffer.reset()

                else:
                    await websocket.send_json(
                        {"type": "error", "message": f"Unknown type: {msg_type}"}
                    )

            elif "bytes" in message:
                buffer.append(message["bytes"])

    except WebSocketDisconnect:
        pass
    except Exception as exc:
        try:
            await websocket.send_json({"type": "error", "message": str(exc)})
        except Exception:
            pass


if __name__ == "__main__":
    uvicorn.run("main:app", host="0.0.0.0", port=8000, reload=True)
