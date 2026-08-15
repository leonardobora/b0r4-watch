"""Music control: ytmusicapi, media keys, playback state."""


async def play(query: str | None = None) -> str:
    """Start or resume playback."""
    if query:
        return f"Tocando: {query}"
    return "Retomando reprodução"


async def next_track() -> str:
    return "Próxima faixa"


async def pause() -> str:
    return "Pausado"
