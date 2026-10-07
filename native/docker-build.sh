#!/usr/bin/env bash
set -e

IMAGE_NAME="segybuild"

echo "=== Building Docker image ($IMAGE_NAME) ==="
docker build -t $IMAGE_NAME .

echo "=== Starting build container ==="
docker run --rm -it -v "$PWD:/src" $IMAGE_NAME /bin/bash -c "cd /src && ./build.sh"

echo "=== Build completed successfully ==="
