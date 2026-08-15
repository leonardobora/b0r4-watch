"""Server configuration loaded from environment variables."""

import os

from dotenv import load_dotenv

load_dotenv()


class Settings:
    """Server settings."""

    openai_api_key: str | None = os.getenv("OPENAI_API_KEY")
    anthropic_api_key: str | None = os.getenv("ANTHROPIC_API_KEY")

    ytmusic_headers_json: str | None = os.getenv("YTMUSIC_HEADERS_JSON")

    watch_auth_token: str = os.getenv("WATCH_AUTH_TOKEN", "change-me")

    mqtt_broker_host: str = os.getenv("MQTT_BROKER_HOST", "localhost")
    mqtt_broker_port: int = int(os.getenv("MQTT_BROKER_PORT", "1883"))

    asr_provider: str = os.getenv("ASR_PROVIDER", "mock")
    llm_provider: str = os.getenv("LLM_PROVIDER", "mock")
    tts_provider: str = os.getenv("TTS_PROVIDER", "mock")

    assistant_language: str = os.getenv("ASSISTANT_LANGUAGE", "pt-BR")

    mock_asr_text: str = os.getenv("MOCK_ASR_TEXT", "tocar música")
    mock_llm_text: str = os.getenv("MOCK_LLM_TEXT", "Claro, vou tocar sua música.")


settings = Settings()
