#!/bin/bash

docker build . -t watcher-builder
docker run \
    --volume $(pwd):/external \
    watcher-builder \
    /bin/bash -c "go build -C /external/watcher-golang -o /external/watcher && chmod +s /external/watcher"
