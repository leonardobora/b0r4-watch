"""Automatic Speech Recognition providers."""

import io

from .config import settings


class BaseASR:
    async def transcribe(self, audio_bytes: bytes, language: str | None = None) -> str:
        raise NotImplementedError


class MockASR(BaseASR):
    async def transcribe(self, audio_bytes: bytes, language: str | None = None) -> str:
        return settings.mock_asr_text


class OpenAIASR(BaseASR):
    def __init__(self):
        from openai import AsyncOpenAI

        self._client = AsyncOpenAI(api_key=settings.openai_api_key)

    async def transcribe(self, audio_bytes: bytes, language: str | None = None) -> str:
        response = await self._client.audio.transcriptions.create(
            model="whisper-1",
            file=("audio.wav", io.BytesIO(audio_bytes)),
            language=language,
        )
        return response.text


def get_asr_provider() -> BaseASR:
    if settings.asr_provider == "openai":
        return OpenAIASR()
    return MockASR()
