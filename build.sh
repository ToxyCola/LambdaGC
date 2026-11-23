#!/bin/sh
# LambdaGC builder for Linux / macOS (see README.md)
cd "$(dirname "$0")" && exec python3 build.py "$@"
