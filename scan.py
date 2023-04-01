import argparse
import asyncio
from clamdavdb import ClamAVDB, ClamAVDBFileMetadata, NotRegularFileError
import os
import time
import sys

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


async def main():
    cpu_count = os.cpu_count()
    assert cpu_count is not None
    max_scans_default = 2 * (cpu_count + 2)
    here = os.path.dirname(__file__)
    parser = argparse.ArgumentParser()
    parser.add_argument('-d', '--db-path', default=os.path.join(here, "clamav-scan-results.sqlite3"))
    parser.add_argument('-m', '--max-scans', default=max_scans_default)
    parser.add_argument('path')

    args = parser.parse_args()
    clamavdb = ClamAVDB(args.db_path)
    await clamavdb.init_scanner()

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
    while len(files_to_scan) > 0 or len(tasks) > 0:
        # TODO
        while len(tasks) < args.max_scans and len(files_to_scan) > 0:
            path = files_to_scan.pop()
            task = asyncio.create_task(clamavdb.scan_file(path))
            tasks.add(task)

        completed, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        tasks = pending
        for task in completed:
            try:
                result:ClamAVDBFileMetadata = await task
                logger.log(str(result))
            except NotRegularFileError as e:
                logger.log(f"Not Regular File: {e}", force=True)
            except FileNotFoundError as e:
                logger.log(f"File Not Found: {e}", force=True)







if __name__ == '__main__':
    asyncio.run(main())