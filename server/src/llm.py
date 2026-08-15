"""LLM providers with tool calling."""

import json

from .config import settings
from .tools import TOOLS, handle_tool_call


class BaseLLM:
    async def chat(self, user_message: str, language: str) -> dict:
        """Return {'text': str, 'tool_calls': list}."""
        raise NotImplementedError


class MockLLM(BaseLLM):
    async def chat(self, user_message: str, language: str) -> dict:
        return {"text": settings.mock_llm_text, "tool_calls": []}


class OpenAILLM(BaseLLM):
    def __init__(self):
        from openai import AsyncOpenAI

        self._client = AsyncOpenAI(api_key=settings.openai_api_key)

    async def chat(self, user_message: str, language: str) -> dict:
        system_prompt = (
            "Você é o assistente do b0r4-watch, um smartwatch pessoal. "
            f"Responda em {language}. Seja conciso, direto e útil no pulso. "
            "Use as ferramentas disponíveis quando necessário."
        )
        messages = [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_message},
        ]

        response = await self._client.chat.completions.create(
            model="gpt-4o-mini",
            messages=messages,
            tools=TOOLS,
            tool_choice="auto",
        )

        message = response.choices[0].message
        text = message.content or ""
        tool_calls = []

        if message.tool_calls:
            for tc in message.tool_calls:
                result = await handle_tool_call(
                    tc.function.name, json.loads(tc.function.arguments)
                )
                tool_calls.append(
                    {
                        "name": tc.function.name,
                        "arguments": tc.function.arguments,
                        "result": result,
                    }
                )

        return {"text": text, "tool_calls": tool_calls}


def get_llm_provider() -> BaseLLM:
    if settings.llm_provider == "openai":
        return OpenAILLM()
    return MockLLM()
