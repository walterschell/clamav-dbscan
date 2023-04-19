#!/bin/bash

docker build . -t watcher-builder
docker run \
    --volume $(pwd):/external \
    watcher-builder \
    /bin/bash -c "cmake -DCMAKE_BUILD_TYPE=Release -S /external/watcher-src -B /build && cmake --build /build && cp /build/watcher /external/watcher && chmod +s /external/watcher"
cp /tmp/build-watcher/watcher 
rm -rf /tmp/build-watcher