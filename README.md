# clamav-dbscan
ClamAV scanning with caching of results

# Usage

The normal use case is to start `watcher` run `scan.py` with the `-w` and `path` arguments specifed while the mirroring software is running. Then once the mirror is complete, run `scan.py` with only the `path` argument specified on the filesystem, and use those results as the AV Scan to examine prior to transfer.


## scan.py
```
$ python3 scan.py --help
usage: scan.py [-h] [-d DB_PATH] [-m MAX_SCANS] [-w] [path]

positional arguments:
  path

options:
  -h, --help            show this help message and exit
  -d DB_PATH, --db-path DB_PATH
  -m MAX_SCANS, --max-scans MAX_SCANS
  -w, --watch           Watch for changes and scan on demand
```

At least one of `-w` or `path` must be specified, however both may be specfied
If `path` is specified the scanner will craw that path.  
if `watch` is specified, the scanner will attempt to connected the filesystem watcher to look for files to scan. The program will not terminate and must be ended with `ctl-c`. Because of filesystem turnover, files are not queued for scan immeadiatly, but are put on a 10s delay to account for temporary files that are deleted as soon as they are closed, or for in progress files that are moved to their final location as soon as they are closed.

## watcher

```
$ ./watcher
Usage: ./watcher <path>
```

`watcher` must be run as `root` takes only one argument, the path to watch.
All files watched must be on the same filesystem as the top level path. It should be starteed before running `scan.py`

# Compiling watcher

## Automated Way
This method will do all of the building inside a docker container without needing to install anything additional on the system. It also has the added benifit of making the watcher binary setuid so it does
not need to be invoked using sudo.

1. Ensure `docker` is installed.
2. Run `./build-watcher.sh`
3. Upon completion the watcher binary will be in the directory

## Manual Way

1. Install dependencies
```bash
sudo apt-get update
apt-get install build-essential cmake
```

2. Configure build
```bash
cmake -DCMAKE_BUILD_TYPE=Release -S watcher-src -B build
```

3. Build the binary
```bash
cmake --build build
```

4. Copy the binary to top level
```bash
cp build/watcher .
```

5. (Optional) Make the binary setuid
```bash
sudo chown root watcher
```
