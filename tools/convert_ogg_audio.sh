#!/usr/bin/env bash
set -euo pipefail

root="${1:-.}"

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg is required. Install it with Homebrew: brew install ffmpeg" >&2
  exit 1
fi

find "$root" -type f -iname '*.ogg' -print0 | while IFS= read -r -d '' ogg; do
  out="${ogg%.*}.mp3"
  if [[ -f "$out" ]]; then
    echo "exists: $out"
    continue
  fi
  echo "convert: $ogg"
  ffmpeg -hide_banner -loglevel error -y -i "$ogg" -codec:a libmp3lame -q:a 2 "$out"
done
