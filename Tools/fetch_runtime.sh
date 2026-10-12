#!/usr/bin/env bash
# Downloads the pinned portable Python (python-build-standalone) and uv for Linux into ThirdParty/,
# verifying SHA-256. Release archives already include these; run this after cloning the repo.
#   ./Tools/fetch_runtime.sh [--force]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$ROOT/ThirdParty"
DL="$TP/.downloads"
PBS_TAG=20261009
PY_VER=3.12.15
UV_VER=0.13.0
PY_FILE="cpython-$PY_VER+$PBS_TAG-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
PY_SHA=ffa8f85f1b56e88b687f08d743d817015524b86a9365fd921243132b80f1a36d
UV_FILE=uv-x86_64-unknown-linux-gnu.tar.gz
UV_SHA=1468ebd5a5541121837c5a2817b9972ba6090fa6caa3d142620850a47fb75154
WANT="python $PY_VER ($PBS_TAG), uv $UV_VER"

PY_DIR="$TP/python/Linux"
UV_DIR="$TP/uv/Linux"
if [[ "${1:-}" != "--force" && -f "$PY_DIR/.runtime-version" && "$(cat "$PY_DIR/.runtime-version")" == "$WANT" && -x "$UV_DIR/uv" ]]; then
  echo "Linux up to date: $WANT"; exit 0
fi

fetch() { # url file sha
  mkdir -p "$DL"
  local path="$DL/$2"
  if [[ ! -f "$path" ]] || ! echo "$3  $path" | sha256sum -c --status; then
    echo "  downloading $2"
    curl -fsSL -o "$path" "$1/${2//+/%2B}"
  fi
  echo "$3  $path" | sha256sum -c --status || { echo "SHA-256 mismatch for $2" >&2; exit 1; }
  echo "$path"
}

PY_TGZ=$(fetch "https://github.com/astral-sh/python-build-standalone/releases/download/$PBS_TAG" "$PY_FILE" "$PY_SHA" | tail -1)
UV_TGZ=$(fetch "https://github.com/astral-sh/uv/releases/download/$UV_VER" "$UV_FILE" "$UV_SHA" | tail -1)
rm -rf "$PY_DIR" "$UV_DIR"; mkdir -p "$PY_DIR" "$UV_DIR"
tar -xzf "$PY_TGZ" -C "$PY_DIR" --strip-components=1
tar -xzf "$UV_TGZ" -C "$UV_DIR" --strip-components=1
chmod +x "$PY_DIR/bin/python3.12" "$UV_DIR/uv"
printf '%s' "$WANT" > "$PY_DIR/.runtime-version"
echo "Linux -> $PY_DIR, $UV_DIR"
