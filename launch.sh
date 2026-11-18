#!/bin/sh

PAK_DIR="$(cd "$(dirname "$0")" && pwd)"
PAK_NAME="$(basename "$PAK_DIR" .pak)"

if [ "$PLATFORM" = "tg3040" ] && [ -z "$DEVICE" ]; then
	export DEVICE="brick"
	export PLATFORM="tg5040"
fi

if [ -n "$LOGS_PATH" ]; then
	mkdir -p "$LOGS_PATH"
	LOG_FILE="$LOGS_PATH/$PAK_NAME.txt"
	rm -f "$LOG_FILE"
	exec > "$LOG_FILE" 2>&1
fi

export PATH="$PAK_DIR/bin/$PLATFORM:$PATH"
chmod +x "$PAK_DIR/bin/$PLATFORM/"* 2>/dev/null || true

MENU_FILE="/tmp/${PAK_NAME}-menu.txt"
OUT_FILE="/tmp/${PAK_NAME}-choice.out"
SYNC_LOG="/tmp/${PAK_NAME}-sync.log"
FIFO_PATH="/tmp/show2.fifo"
SHOW2_PID=""
PRESENTER_PID=""

stop_progress_ui() {
	if [ -n "$SHOW2_PID" ]; then
		if [ -p "$FIFO_PATH" ] && kill -0 "$SHOW2_PID" 2>/dev/null; then
			echo "QUIT" > "$FIFO_PATH" &
			QUIT_PID=$!
			usleep 200000 2>/dev/null || sleep 1
			kill "$QUIT_PID" 2>/dev/null || true
		fi
		kill "$SHOW2_PID" 2>/dev/null || true
		wait "$SHOW2_PID" 2>/dev/null || true
		SHOW2_PID=""
		rm -f "$FIFO_PATH"
	fi
	if [ -n "$PRESENTER_PID" ]; then
		kill "$PRESENTER_PID" 2>/dev/null || true
		wait "$PRESENTER_PID" 2>/dev/null || true
		PRESENTER_PID=""
	fi
	killall minui-presenter >/dev/null 2>&1 || true
}

cleanup() {
	stop_progress_ui
	rm -f "$MENU_FILE" "$OUT_FILE" "$SYNC_LOG" /tmp/stay_awake
}

echo "1" > /tmp/stay_awake
trap cleanup EXIT INT TERM HUP QUIT

echo "Starting $PAK_NAME on platform=$PLATFORM"

BIN="$PAK_DIR/bin/$PLATFORM/ra-cache"
if [ ! -x "$BIN" ]; then
	echo "Error: binary not found for platform '$PLATFORM': $BIN"
	minui-presenter --message "Binary not found for platform '$PLATFORM'" --timeout 0 --confirm-show --confirm-text "OK" >/dev/null 2>&1 || true
	exit 1
fi

STATUS_LINE="$("$BIN" --status)"
STATUS_RC=$?
if [ "$STATUS_RC" -eq 2 ]; then
	minui-presenter --message "${STATUS_LINE:-Not logged in (configure RA in NextUI Settings)}" --timeout 0 --confirm-show --confirm-text "OK"
	exit 0
elif [ "$STATUS_RC" -ne 0 ]; then
	minui-presenter --message "Failed to read RA status (exit $STATUS_RC)" --timeout 0 --confirm-show --confirm-text "OK"
	exit 1
fi

cat << 'EOF' > "$MENU_FILE"
Sync New Games
Refresh Sessions
Force Rebuild Cache
Exit
EOF

while true; do
	STATUS_LINE="$("$BIN" --status)" || break
	rm -f "$OUT_FILE"
	minui-list --disable-auto-sleep --file "$MENU_FILE" --format text --title "$STATUS_LINE" --confirm-text "SELECT" --write-location "$OUT_FILE" >/dev/null
	RC=$?
	if [ "$RC" -ne 0 ] || [ ! -f "$OUT_FILE" ]; then
		break
	fi

	CHOICE="$(cat "$OUT_FILE")"
	case "$CHOICE" in
		"Sync New Games")
			FLAG=""
			;;
		"Refresh Sessions")
			FLAG="--refresh-sessions"
			;;
		"Force Rebuild Cache")
			FLAG="--force"
			;;
		"Exit"|"")
			break
			;;
		*)
			break
			;;
	esac

	if command -v show2.elf >/dev/null 2>&1; then
		LOGO_PATH=""
		if [ -n "$SDCARD_PATH" ] && [ -f "$SDCARD_PATH/.system/res/logo.png" ]; then
			LOGO_PATH="$SDCARD_PATH/.system/res/logo.png"
		fi
		rm -f "$FIFO_PATH"
		show2.elf --mode=daemon --image="$LOGO_PATH" --text="Starting RA Cache..." --progress=0 >/dev/null 2>&1 &
		SHOW2_PID=$!
		wait_i=0
		while [ ! -p "$FIFO_PATH" ] && [ "$wait_i" -lt 20 ]; do
			usleep 100000 2>/dev/null || sleep 1
			wait_i=$((wait_i + 1))
		done
	else
		minui-presenter --message "Running..." --timeout -1 >/dev/null 2>&1 &
		PRESENTER_PID=$!
	fi

	: > "$SYNC_LOG"
	if [ -n "$FLAG" ]; then
		"$BIN" --fifo "$FIFO_PATH" "$FLAG" > "$SYNC_LOG" 2>&1
	else
		"$BIN" --fifo "$FIFO_PATH" > "$SYNC_LOG" 2>&1
	fi
	SYNC_RC=$?
	cat "$SYNC_LOG"

	stop_progress_ui

	SUMMARY="$(grep '^SUMMARY:' "$SYNC_LOG" | tail -n 1 | sed 's/^SUMMARY: //')"
	if [ -z "$SUMMARY" ]; then
		SUMMARY="$(tail -n 1 "$SYNC_LOG")"
		if [ -z "$SUMMARY" ]; then
			SUMMARY="Sync failed (exit $SYNC_RC)"
		fi
	fi

	minui-presenter --message "$SUMMARY" --timeout 0 --confirm-show --confirm-text "OK"
done

exit 0
