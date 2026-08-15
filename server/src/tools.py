"""Tool definitions and handlers for the assistant."""

from . import lights, music


TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "music_play",
            "description": "Play music by query or resume playback.",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {
                        "type": "string",
                        "description": "Song, artist or playlist name.",
                    }
                },
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "music_next",
            "description": "Skip to next track.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "music_pause",
            "description": "Pause playback.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
    {
        "type": "function",
        "function": {
            "name": "lights_toggle",
            "description": "Toggle lights in a room.",
            "parameters": {
                "type": "object",
                "properties": {
                    "room": {
                        "type": "string",
                        "description": "Room name, e.g. 'sala', 'quarto'.",
                    }
                },
                "required": ["room"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "system_time",
            "description": "Get current time.",
            "parameters": {"type": "object", "properties": {}},
        },
    },
]


async def handle_tool_call(name: str, arguments: dict) -> str:
    if name == "music_play":
        return await music.play(arguments.get("query"))
    if name == "music_next":
        return await music.next_track()
    if name == "music_pause":
        return await music.pause()
    if name == "lights_toggle":
        return await lights.toggle(arguments.get("room", "sala"))
    if name == "system_time":
        import datetime

        return datetime.datetime.now().isoformat()
    return f"Tool {name} not implemented"
