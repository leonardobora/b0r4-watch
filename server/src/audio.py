"""Audio buffer management for WebSocket streams."""

import io
import wave


class AudioBuffer:
    """Accumulates raw PCM audio chunks and produces a WAV file in memory."""

    def __init__(
        self,
        sample_rate: int = 16000,
        channels: int = 1,
        sample_width: int = 2,
    ):
        self.sample_rate = sample_rate
        self.channels = channels
        self.sample_width = sample_width
        self._chunks: list[bytes] = []

    def append(self, chunk: bytes) -> None:
        self._chunks.append(chunk)

    def to_wav_bytes(self) -> bytes:
        """Return accumulated audio as a WAV file in memory."""
        buffer = io.BytesIO()
        with wave.open(buffer, "wb") as wav_file:
            wav_file.setnchannels(self.channels)
            wav_file.setsampwidth(self.sample_width)
            wav_file.setframerate(self.sample_rate)
            wav_file.writeframes(b"".join(self._chunks))
        buffer.seek(0)
        return buffer.read()

    def reset(self) -> None:
        self._chunks.clear()
