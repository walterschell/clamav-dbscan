import argparse
import asyncio
import fcntl
import json
import os
import socket
import struct
import time
from asyncio import Task
from collections import deque
from typing import TypeVar, Generic, Protocol, Callable, Union, Optional

from clamdavdb import ClamAVDB, ClamAVDBFileMetadata, NotRegularFileError

WATCHER_UNIX_DOMAIN_SOCKET_PATH = "/tmp/fswatcher.sock"
T = TypeVar("T")

async def read_unix_domain_socket(sock: socket.socket) -> bytes:
    loop = asyncio.get_event_loop()
    result = loop.create_future()

    def on_unix_domain_socket_read_ready():
        SIOCINQ: int = 0x541B # pylint: disable=invalid-name
        raw_data_available = bytearray(struct.calcsize("q"))
        try:
            fcntl.ioctl(sock, SIOCINQ, raw_data_available, True)
            (data_available,) = struct.unpack("q", raw_data_available)
            # print(f"Attempting to read {data_available} bytes")
            data = sock.recv(data_available)
            result.set_result(data)
        except Exception as e: # pylint: disable=broad-except
            result.set_exception(e)

        loop.remove_reader(sock.fileno())

    loop.add_reader(sock.fileno(), on_unix_domain_socket_read_ready)
    return await result


async def on_demand_watcher(reconnect=True):
    while True:
        s = None
        while s is None:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
                s.connect(WATCHER_UNIX_DOMAIN_SOCKET_PATH)
                s.setblocking(False)
            except Exception: # pylint: disable=broad-except
                s = None
                if not reconnect:
                    return
                await asyncio.sleep(1)
        while True:
            data = await read_unix_domain_socket(s)
            if data == b"":
                if not reconnect:
                    return
                break
            print(data.decode())


if __name__ == "__main__":
    asyncio.run(on_demand_watcher())