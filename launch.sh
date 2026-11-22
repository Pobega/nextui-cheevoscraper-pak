#!/bin/sh

PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
PAK_NAME="$(basename "$PAK_DIR" .pak)"

if [ -n "$LOGS_PATH" ]; then
	mkdir -p "$LOGS_PATH"
	LOG_FILE="$LOGS_PATH/$PAK_NAME.txt"
	exec > "$LOG_FILE" 2>&1
fi

cleanup() {
	rm -f /tmp/stay_awake
}

echo "1" > /tmp/stay_awake
trap cleanup EXIT INT TERM HUP QUIT

echo "Starting $PAK_NAME on platform=$PLATFORM"

UI_BIN="$PAK_DIR/bin/$PLATFORM/cheevoscraper-ui"
RA_BIN="$PAK_DIR/bin/$PLATFORM/ra-cache"

if [ ! -x "$UI_BIN" ] || [ ! -x "$RA_BIN" ]; then
	echo "Error: missing binaries for platform '$PLATFORM'"
	exit 1
fi

export PATH="$PAK_DIR/bin/$PLATFORM:$PATH"
"$UI_BIN"
RC=$?
echo "$PAK_NAME exited with code $RC"
exit "$RC"
