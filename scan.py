import argparse
import asyncio
from clamdavdb import ClamAVDB, ClamAVDBFileMetadata, NotRegularFileError
import os
import time
from asyncio import Task
import socket
import fcntl
import struct
import json

WATCHER_UNIX_DOMAIN_SOCKET_PATH = "/tmp/fswatcher.sock"


def sec_to_interval_string(sec: int) -> str:
    days_left, sec_left = divmod(sec, 60*60*24)
    hours_left, sec_left = divmod(sec_left, 3600)
    min_left, sec_left = divmod(sec_left, 60)
    if days_left > 0:
        return f"{days_left}d {hours_left}h {min_left}m"
    elif hours_left > 0:
        return f"{hours_left}h {min_left}m"
    elif min_left > 0:
        return f"{min_left}m"
    return f"{sec_left}s"

class Logger:
    def __init__(self, total: int) -> None:
        self.total = total
        self.count = 0
        self.last_output = None
        self.last_count = None

    def log(self, message: str, *, force=False):
        self.count += 1
        now = time.time()
        if force or (self.last_output is None or (now > self.last_output + 10.0)):
            pct_complete = 100 * self.count / self.total
            speed = 'UNK'
            time_remaining = "UNK"
            if self.last_count is not None:
                assert self.last_output is not None
                items_delta = self.count - self.last_count
                time_delta = now - self.last_output
                items_per_sec = int(items_delta / time_delta)
                speed = f"{items_per_sec} Files/Sec"
                if items_per_sec > 0:
                    item_remaining = self.total - self.count
                    seconds_remaining = int(item_remaining / items_per_sec)
                    time_remaining = sec_to_interval_string(seconds_remaining)
            print(f"{message} ({self.count} {pct_complete:.1f}% - {speed} EST Completion: {time_remaining})")
            self.last_output = now
            self.last_count = self.count


async def read_unix_domain_socket(sock: socket.socket) -> bytes:
    loop = asyncio.get_event_loop()
    result = loop.create_future()
    def on_unix_domain_socket_read_ready():
        SIOCINQ:int =  0x541B
        raw_data_available = bytearray(struct.calcsize('q'))
        try:
            fcntl.ioctl(sock, SIOCINQ, raw_data_available, True)
            data_available, = struct.unpack('q', raw_data_available)
            print(f"Attempting to read {data_available} bytes")
            data = sock.recv(data_available)
            result.set_result(data)
        except Exception as e:
            result.set_exception(e)

        loop.remove_reader(sock.fileno())

    loop.add_reader(sock.fileno(), on_unix_domain_socket_read_ready)
    return await result

class DeferredScanManager:
    def __init__(self, clamavdb: ClamAVDB) -> None:
        self.scans: dict[str, Task] = {}
        self.paths_by_scan: dict[Task, str] = {}
        self.clamavdb = clamavdb
        self.reaper_task = None

    async def start(self):
        async def reaper():
            while True:

                completed, _ = await asyncio.wait(self.scans.values(), return_when=asyncio.FIRST_COMPLETED)
                for task in completed:
                    try:
                        await task
                    except Exception as e:
                        print(f"Error scanning {self.paths_by_scan[task]}: {type(e)} - {e}")
                    del self.scans[self.paths_by_scan[task]]
                    del self.paths_by_scan[task]

        self.reaper_task = asyncio.create_task(reaper())

    


    async def schedule_file_scan(self, file_path: str):
        if file_path in self.scans:
            return
        task = asyncio.create_task(self.deferred_scan_file(file_path))
        self.scans[file_path] = task
        self.paths_by_scan[task] = file_path

    async def cancel_file_scan(self, file_path: str):
        if file_path in self.scans:
            self.scans[file_path].cancel()


    async def deferred_scan_file(self, file_path: str):
        print(f"Will scan {file_path} after 10s delay")
        try:
            await asyncio.sleep(10)

            try:
                metadata = await self.clamavdb.scan_file(file_path)
                print(f"Scanned {file_path} - {metadata}")
            except NotRegularFileError:
                print(f"Skipping {file_path} - Not a regular file")
        except asyncio.CancelledError:
            print(f"Cancelled scan of {file_path}")


async def on_demand_watcher(clamavdb: ClamAVDB):
    scan_manager = DeferredScanManager(clamavdb)
    await scan_manager.start()
    s = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    s.connect(WATCHER_UNIX_DOMAIN_SOCKET_PATH)
    s.setblocking(False)
    while True:
        data = await read_unix_domain_socket(s)
        if data == b'':
            break
        watch_data = json.loads(data.decode('utf-8'))
        for file_event_dict in watch_data['events']:
            path = file_event_dict['path']
            operation = file_event_dict['operation']
            if operation == 'close_for_write' or operation == 'move_to':
                await scan_manager.schedule_file_scan(path)
            elif operation == 'remove' or operation == 'move_from':
                await scan_manager.cancel_file_scan(path)
                clamavdb.remove_file(path)


async def main():
    cpu_count = os.cpu_count()
    assert cpu_count is not None
    max_scans_default = 2 * (cpu_count + 2)
    here = os.path.dirname(__file__)
    parser = argparse.ArgumentParser()
    parser.add_argument('-d', '--db-path', default=os.path.join(here, "clamav-scan-results.sqlite3"))
    parser.add_argument('-m', '--max-scans', default=max_scans_default)
    parser.add_argument('-w', '--watch', action='store_true', help="Watch for changes and scan on demand")
    parser.add_argument('path', nargs='?', default=None)

    args = parser.parse_args()

    if not args.watch and args.path is None:
        parser.error("Must specify path to scan, or use --watch to watch for changes")
    


    clamavdb = ClamAVDB(args.db_path)
    await clamavdb.init_scanner()

    if args.watch:
        await on_demand_watcher(clamavdb)
        return

    topdev = os.stat(args.path).st_dev


    files_to_scan = []
    print("Gathering files")
    for dirpath, dirnames, filenames in os.walk(args.path):
        for filename in filenames:
            relpath = os.path.join(dirpath, filename)
            fullpath = os.path.abspath(relpath)
            files_to_scan.append(fullpath)
        to_remove = []
        for dirname in dirnames:
            subdirpath = os.path.join(dirpath, dirname)
            if os.lstat(subdirpath).st_dev != topdev:
                print(f"Will not scan {subdirpath}. On different filesystem")
                to_remove.append(dirname)
        for dirname in to_remove:
            dirnames.remove(dirname)
    print("Gathering complete. Scanning Files...")
    logger = Logger(len(files_to_scan))

    tasks = set()

    ok_fh = open("clamav-ok.txt", 'w')
    infected_fh = open("clamav-infected.txt", 'w')
    error_fh = open("clamav-error.txt", 'w')
    fs_error_fh = open("clamav-fs-error.txt", 'w')

    while len(files_to_scan) > 0 or len(tasks) > 0:
        # TODO
        while len(tasks) < args.max_scans and len(files_to_scan) > 0:
            path = files_to_scan.pop()
            task = asyncio.create_task(clamavdb.scan_file(path))
            tasks.add(task)

        completed, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        tasks = pending
        task: Task[ClamAVDBFileMetadata]
        for task in completed:
            try:
                result:ClamAVDBFileMetadata = await task
                logger.log(str(result))
                if result.status == 'OK':
                    print(result.log_line(), file=ok_fh)
                elif result.status == 'FOUND':
                    print(result.log_line(), file=infected_fh)
                else:
                    assert result.status == 'ERROR'
                    print(result.log_line(), file=error_fh)

            except NotRegularFileError as e:
                msg = f"Not Regular File: {e}"
                logger.log(msg, force=True)
                print(msg, file=fs_error_fh)
            except FileNotFoundError as e:
                msg = f"File Not Found: {e}"
                logger.log(msg, force=True)
                print(msg, file=fs_error_fh)







if __name__ == '__main__':
    asyncio.run(main())