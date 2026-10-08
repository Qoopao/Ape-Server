#!/usr/bin/env python3
"""Test-only MySQL TCP proxy: discard exactly one COMMIT response after receiving it.
No database configuration or application files are changed. SSL must be disabled.
"""
import asyncio

used = False

async def packet(reader):
    header = await reader.readexactly(4)
    size = int.from_bytes(header[:3], 'little')
    return header + await reader.readexactly(size)

async def serve(client_r, client_w):
    global used
    upstream_r, upstream_w = await asyncio.open_connection('127.0.0.1', 33316)
    state = {'drop': False}
    async def forward_requests():
        global used
        while True:
            data = await packet(client_r)
            if data[4:5] == b'\x03' and data[5:].strip().upper() == b'COMMIT' and not used:
                used = True
                state['drop'] = True
            upstream_w.write(data)
            await upstream_w.drain()
    async def forward_responses():
        while True:
            data = await packet(upstream_r)
            if state['drop']:
                assert data[4] == 0, 'Expected successful COMMIT acknowledgement'
                print('DROPPED_COMMIT_ACK', flush=True)
                return
            client_w.write(data)
            await client_w.drain()
    tasks = [asyncio.create_task(forward_requests()), asyncio.create_task(forward_responses())]
    try:
        done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        for task in done:
            try: task.result()
            except (asyncio.IncompleteReadError, ConnectionError): pass
    finally:
        for task in tasks: task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        client_w.close(); upstream_w.close()

async def main():
    server = await asyncio.start_server(serve, '127.0.0.1', 33317)
    print('READY', flush=True)
    async with server: await server.serve_forever()

asyncio.run(main())