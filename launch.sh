#!/bin/sh

PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
PAK_NAME="$(basename "$PAK_DIR" .pak)"
FIFO_PATH="/tmp/show2.fifo"
SHOW2_PID=""

if [ -n "$LOGS_PATH" ]; then
	mkdir -p "$LOGS_PATH"
	LOG_FILE="$LOGS_PATH/$PAK_NAME.txt"
	exec > "$LOG_FILE" 2>&1
fi

echo "Starting $PAK_NAME on platform=$PLATFORM"

BIN="$PAK_DIR/bin/$PLATFORM/ra-cache"
if [ ! -x "$BIN" ]; then
	echo "Error: binary not found for platform '$PLATFORM': $BIN"
	if command -v show2.elf >/dev/null 2>&1; then
		show2.elf --mode=simple --text="Missing binary for $PLATFORM" --timeout=3 >/dev/null 2>&1
	fi
	exit 1
fi

cleanup() {
	if [ -n "$SHOW2_PID" ] && kill -0 "$SHOW2_PID" 2>/dev/null; then
		printf 'QUIT\n' > "$FIFO_PATH" 2>/dev/null || true
		wait "$SHOW2_PID" 2>/dev/null || true
	fi
	rm -f "$FIFO_PATH"
}
trap cleanup EXIT INT TERM

if command -v show2.elf >/dev/null 2>&1; then
	LOGO_PATH=""
	if [ -n "$SDCARD_PATH" ] && [ -f "$SDCARD_PATH/.system/res/logo.png" ]; then
		LOGO_PATH="$SDCARD_PATH/.system/res/logo.png"
	fi
	if [ -n "$LOGO_PATH" ]; then
		show2.elf --mode=daemon --image="$LOGO_PATH" --text="Starting RA Cache..." --progress=0 >/dev/null 2>&1 &
	else
		show2.elf --mode=daemon --text="Starting RA Cache..." --progress=0 >/dev/null 2>&1 &
	fi
	SHOW2_PID=$!

	WAIT_COUNT=0
	while [ ! -p "$FIFO_PATH" ] && [ "$WAIT_COUNT" -lt 20 ]; do
		sleep 0.1
		WAIT_COUNT=$((WAIT_COUNT + 1))
	done
fi

"$BIN" --fifo "$FIFO_PATH"
RC=$?

if [ -n "$SHOW2_PID" ] && kill -0 "$SHOW2_PID" 2>/dev/null; then
	if [ "$RC" -ne 0 ]; then
		printf 'TEXT:RA Cache failed (exit %d)\n' "$RC" > "$FIFO_PATH" 2>/dev/null || true
	fi
	sleep 2
	printf 'QUIT\n' > "$FIFO_PATH" 2>/dev/null || true
	wait "$SHOW2_PID" 2>/dev/null || true
	SHOW2_PID=""
fi

exit "$RC"
