#!/bin/sh
# LambdaGC builder for Linux / macOS. Needs Half-Life 2 on Steam (steam_legacy branch) and an internet
# connection; build.py fetches everything else into .tools and builds LambdaGC.iso.
cd "$(dirname "$0")" || exit 1
need_python() {
  command -v python3 >/dev/null 2>&1 && python3 -c 'import sys, ensurepip; sys.exit(sys.version_info < (3, 9))' 2>/dev/null
}
if ! need_python; then
  echo "[LambdaGC] installing Python 3 (your password may be asked for) ..."
  if [ "$(uname)" = Darwin ]; then
    if command -v brew >/dev/null 2>&1; then brew install python3
    else xcode-select --install 2>/dev/null; echo "Install Python 3 from https://www.python.org/downloads/ and run ./build.sh again."; exit 1; fi
  elif command -v apt-get >/dev/null 2>&1; then sudo apt-get update && sudo apt-get install -y python3 python3-venv python3-pip
  elif command -v dnf >/dev/null 2>&1; then sudo dnf install -y python3 python3-pip
  elif command -v pacman >/dev/null 2>&1; then sudo pacman -S --needed --noconfirm python python-pip
  elif command -v zypper >/dev/null 2>&1; then sudo zypper install -y python3 python3-pip
  else echo "Install Python 3.9+ with your package manager and run ./build.sh again."; exit 1; fi
fi
# a private environment in .tools/venv for the Python packages
if [ ! -x .tools/venv/bin/python ]; then
  mkdir -p .tools && python3 -m venv .tools/venv || { echo "[LambdaGC] couldn't create a Python environment (on Debian/Ubuntu: sudo apt-get install python3-venv)"; exit 1; }
fi
exec .tools/venv/bin/python build.py "$@"
