"""Text-to-Speech providers."""

import io

from .config import settings


class BaseTTS:
    async def synthesize(self, text: str, language: str) -> bytes:
        """Return audio bytes."""
        raise NotImplementedError


class MockTTS(BaseTTS):
    async def synthesize(self, text: str, language: str) -> bytes:
        """Return a minimal silent WAV."""
        import wave

        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(16000)
            wav.writeframes(b"\x00\x00" * 16000)  # 1 second of silence
        buffer.seek(0)
        return buffer.read()


class EdgeTTS(BaseTTS):
    async def synthesize(self, text: str, language: str) -> bytes:
        import edge_tts

        voice = (
            "pt-BR-AntonioNeural"
            if language.startswith("pt")
            else "en-US-GuyNeural"
        )
        communicate = edge_tts.Communicate(text, voice)
        buffer = io.BytesIO()
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                buffer.write(chunk["data"])
        buffer.seek(0)
        return buffer.read()


class OpenAITTS(BaseTTS):
    def __init__(self):
        from openai import AsyncOpenAI

        self._client = AsyncOpenAI(api_key=settings.openai_api_key)

    async def synthesize(self, text: str, language: str) -> bytes:
        response = await self._client.audio.speech.create(
            model="tts-1",
            voice="alloy",
            input=text,
            response_format="mp3",
        )
        return await response.aread()


def get_tts_provider() -> BaseTTS:
    if settings.tts_provider == "openai":
        return OpenAITTS()
    if settings.tts_provider == "edge_tts":
        return EdgeTTS()
    return MockTTS()
