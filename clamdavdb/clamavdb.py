from typing import Optional
import os
import sqlite3
from dataclasses import dataclass
from datetime import datetime
import asyncio
import shlex
import sys
import re
import time
import stat as statconstants
import subprocess
from string import Template

@dataclass
class ClamAVDBFileMetadata:
    path: str
    mtime_epoch: int
    filesize: int
    pulled_from_cache: bool
    status: str
    scanned_at: datetime
    scan_results: str

    def ok(self) -> bool:
        return self.status == 'OK'

    def __str__(self):
        contents = self.scan_results
        return f"{self.path} ({self.status}: {contents})"
    
    def log_line(self) -> str:
        cache_str = "FROM CACHE" if self.pulled_from_cache else "SCANNED"
        return f"{cache_str}:{self.scanned_at}|{self.path}|mtime:{self.mtime_epoch}|{self.filesize} bytes|{self.status}|{self.scan_results}"

def adapt_bool(value: bool) -> bytes:
    return b"true" if value else b"false"

def convert_bool(value: bytes) -> bool:
    if value == b"true":
        return True
    if value == b"false":
        return False
    raise ValueError(value)

sqlite3.register_adapter(bool, adapt_bool)
sqlite3.register_converter("bool", convert_bool)


CLAMD_PATH = "/usr/sbin/clamd"


scan_results_re = re.compile(r"^(.+?):\s(.+?)?\s*(ERROR|OK|FOUND)")

class NotRegularFileError(Exception):
    pass



class ClambScanMgr:
    def __init__(self) -> None:
        self._queue: asyncio.Queue[tuple[str, asyncio.Future]] = asyncio.Queue()
        self.socket_path = f"/tmp/clamavdb-{os.getpid()}.ctl"
        self.clamd = None
        self.clamd_task = None
        self.init_mutex = asyncio.Lock()
        self.clamd_output_enabled=True

    async def ensure_clamd(self):
        if self.clamd is not None:
                return
        async with self.init_mutex:
            if self.clamd is not None:
                return
            here = os.path.dirname(__file__)
            config_fullpath = os.path.join(here, 'clamd.conf')
            with open(config_fullpath) as fh:
                config_template_contents = fh.read()
            config_template = Template(config_template_contents)

            max_threads = os.cpu_count()
            assert max_threads is not None
            max_threads += 2
            config = config_template.safe_substitute(socket_path=self.socket_path, max_threads=max_threads)

            config_fullpath = f"/tmp/clamd-{os.getpid()}.conf"
            with open(config_fullpath, 'w') as fh:
                fh.write(config)

            print("Starting clamd...")
            self.clamd = await asyncio.create_subprocess_exec(CLAMD_PATH, *shlex.split(f"clamd -F -c {config_fullpath}"), cwd=here, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            async def wait_for_process():
                assert self.clamd is not None
                assert self.clamd.stdout is not None
                while not self.clamd.stdout.at_eof():
                    line = (await self.clamd.stdout.readline()).decode().rstrip()
                    if self.clamd_output_enabled:
                        print(line)
                await self.clamd.wait()
                print(f"Error: clamd exited")
                sys.exit()

            self.clamd_task = asyncio.create_task(wait_for_process())
            
            while not os.path.exists(self.socket_path):
                await asyncio.sleep(1.0)
            os.unlink(config_fullpath)
            while True:
                try:
                    reader, writer = await asyncio.open_unix_connection(self.socket_path)
                    writer.write(f"PING\0".encode())
                    line = (await reader.readline()).decode().rstrip()
                    if line == "PONG":
                        break
                except Exception:
                    pass
                print("Sleeping for socket to be working")
                await asyncio.sleep(1.0)

            self.clamd_output_enabled = False




    async def scan(self, path: str, *, stat=None):
        await self.ensure_clamd()
        if stat is None:
            stat = os.lstat(path)
            if not (statconstants.S_ISREG(stat.st_mode) or statconstants.S_ISLNK(stat.st_mode)):
                raise NotRegularFileError(path)

        reader, writer = await asyncio.open_unix_connection(self.socket_path)
        writer.write(f"SCAN {path}\0".encode())
        lines = []
        while not reader.at_eof():
            line = (await reader.readline()).decode().rstrip()
            if line != "":
                lines.append(line)
        firstline = lines[0]
        match = re.match(scan_results_re, firstline)
        if match is None:
            print(f"Couldn't match >{firstline}<")
            import pdb; pdb.set_trace()
        assert match is not None
        filename, inner_message, status = match.groups()
        assert filename == path
        result = ClamAVDBFileMetadata(path, int(stat.st_mtime), stat.st_size, False, status, datetime.fromtimestamp(time.time()), "\n".join(lines))

        # if not result.ok():
        #     print(f"{path}:({status}):{inner_message}")
        return result


class ClamAVDB:

    def __init__(self, db_path: str) -> None:
        self.db = sqlite3.connect(db_path, isolation_level=None, detect_types=sqlite3.PARSE_DECLTYPES)
        self.db.execute("PRAGMA journal_mode=wal")
        self.db.execute("PRAGMA synchronous=OFF")

        with self.db:
            self.db.execute("CREATE TABLE IF NOT EXISTS files(path PRIMARY KEY, mtime_epoch INTEGER NOT NULL, filesize INTEGER NOT NULL, status NOT NULL, scanned_at timestamp NOT NULL, scan_results NOT NULL)")
            self.db.execute("CREATE INDEX IF NOT EXISTS files_status_index ON files(status)")

        self.scanner = ClambScanMgr()

    def __contains__(self, path: str) -> bool:
        try:
            self.__getitem__(path)
            return True
        except KeyError:
            return False

    def __getitem__(self, path) -> ClamAVDBFileMetadata:
        row = self.db.execute("SELECT mtime_epoch, filesize, status, scanned_at, scan_results FROM files WHERE path = ?", (path,)).fetchone()
        if row is None:
            raise KeyError(path)

        mtime_epoch, filesize, status, scanned_at, scan_results = row
        result = ClamAVDBFileMetadata(path, mtime_epoch, filesize,True, status, scanned_at, scan_results)
        return result

    async def init_scanner(self):
        await self.scanner.ensure_clamd()


    async def scan_file(self, path:str, cache_ok=True) -> ClamAVDBFileMetadata:
        stat = os.lstat(path)

        if cache_ok:
            try:
                cached_result = self[path]
                if stat.st_size == cached_result.filesize and int(stat.st_mtime) == cached_result.mtime_epoch:
                    return cached_result
                # print(f"Cache hit on {path}, but metatdat was no good")
            except KeyError:
                # print(f"Cache miss for {path}")
                pass

        # print(f"Scanning {path}")
        result = await self.scanner.scan(path
        )
        assert result.path == path
        with self.db:
            self.db.execute("INSERT OR REPLACE INTO files(path, mtime_epoch, filesize, status, scanned_at, scan_results) VALUES (?,?,?,?,?,?)",
                            (   result.path,
                                result.mtime_epoch,
                                result.filesize,
                                result.status,
                                result.scanned_at,
                                result.scan_results
                            ))
        # print(f"Successfully added {path} to cache")
        return result