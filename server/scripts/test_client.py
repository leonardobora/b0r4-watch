"""Test client for the assistant WebSocket endpoint.

Usage:
    cd server
    uvicorn main:app --reload
    # in another terminal:
    python scripts/test_client.py
"""

import asyncio
import json

import websockets


async def main():
    uri = "ws://localhost:8000/ws/audio"

    async with websockets.connect(uri) as websocket:
        # Start recording
        await websocket.send(json.dumps({"type": "start"}))
        response = await websocket.recv()
        print("Server:", response)

        # Send fake audio (1 second of silence as 16-bit 16kHz mono PCM)
        silence = b"\x00\x00" * 16000
        await websocket.send(silence)

        # End recording
        await websocket.send(json.dumps({"type": "end"}))

        # Receive responses
        for _ in range(10):
            try:
                msg = await asyncio.wait_for(websocket.recv(), timeout=5.0)
                if isinstance(msg, bytes):
                    print(f"Received audio: {len(msg)} bytes")
                else:
                    print(f"Received: {msg}")
            except asyncio.TimeoutError:
                break


if __name__ == "__main__":
    asyncio.run(main())
