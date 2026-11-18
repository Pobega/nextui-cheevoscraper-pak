#!/bin/sh

PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
PAK_NAME="$(basename "$PAK_DIR" .pak)"

if [ -n "$LOGS_PATH" ]; then
	mkdir -p "$LOGS_PATH"
	LOG_FILE="$LOGS_PATH/$PAK_NAME.txt"
	exec > "$LOG_FILE" 2>&1
fi

echo "Starting $PAK_NAME on platform=$PLATFORM"

BIN="$PAK_DIR/bin/$PLATFORM/ra-cache"
if [ ! -x "$BIN" ]; then
	echo "Error: binary not found for platform '$PLATFORM': $BIN"
	exit 1
fi

exec "$BIN" --ui
