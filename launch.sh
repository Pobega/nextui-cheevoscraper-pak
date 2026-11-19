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
SYS_LIST_TSV="/tmp/${PAK_NAME}-systems.tsv"
SYS_MENU_FILE="/tmp/${PAK_NAME}-systems.txt"
SYS_OUT_FILE="/tmp/${PAK_NAME}-system.out"
ACT_MENU_FILE="/tmp/${PAK_NAME}-action.txt"
ACT_OUT_FILE="/tmp/${PAK_NAME}-action.out"
GAMES_JSON="/tmp/${PAK_NAME}-games.json"
GAMES_OUT="/tmp/${PAK_NAME}-games.out"
SETTINGS_JSON="/tmp/${PAK_NAME}-settings.json"
SETTINGS_OUT="/tmp/${PAK_NAME}-settings.out"
SETTINGS_FILE="${USERDATA_PATH:-/tmp}/$PAK_NAME/settings.conf"
BADGES="all"
SKIP_DISC="0"
FIFO_PATH="/tmp/show2.fifo"
SHOW2_PID=""
BUSY_PID=""
STATUS_DIRTY=0
SYS_LIST_CACHED_SKIP_DISC=""
PRESENTER_PID=""

load_settings() {
	BADGES="all"
	SKIP_DISC="0"
	[ -f "$SETTINGS_FILE" ] || return 0
	while IFS='=' read -r s_key s_val; do
		case "$s_key" in
			BADGES)
				case "$s_val" in
					all|unlocked|none) BADGES="$s_val" ;;
				esac
				;;
			SKIP_DISC)
				case "$s_val" in
					0|1) SKIP_DISC="$s_val" ;;
				esac
				;;
		esac
	done < "$SETTINGS_FILE"
}

save_settings() {
	mkdir -p "$(dirname "$SETTINGS_FILE")"
	printf 'BADGES=%s\nSKIP_DISC=%s\n' "$BADGES" "$SKIP_DISC" > "$SETTINGS_FILE"
}

start_busy() {
	stop_busy
	minui-presenter --message "$1" --timeout -1 >/dev/null 2>&1 &
	BUSY_PID=$!
}

stop_busy() {
	if [ -n "$BUSY_PID" ] && kill -0 "$BUSY_PID" 2>/dev/null; then
		kill -9 "$BUSY_PID" 2>/dev/null || true
		wait "$BUSY_PID" 2>/dev/null || true
	fi
	BUSY_PID=""
}

stop_progress_ui() {
	stop_busy
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
		kill -9 "$PRESENTER_PID" 2>/dev/null || true
		wait "$PRESENTER_PID" 2>/dev/null || true
		PRESENTER_PID=""
	fi
	killall -9 minui-presenter >/dev/null 2>&1 || true
}

cleanup() {
	stop_progress_ui
	rm -f "/tmp/${PAK_NAME}-"* /tmp/stay_awake
}

confirm_action() {
	minui-presenter --message "$1" --timeout 0 --confirm-show --confirm-text "CONTINUE" --cancel-show --cancel-text "CANCEL" >/dev/null 2>&1
}

run_sync_with_ui() {
	INIT_MSG="Syncing games..."
	case " $* " in
		*" --refresh-sessions "*) INIT_MSG="Refreshing sessions..." ;;
		*" --force "*)            INIT_MSG="Rebuilding cache..." ;;
	esac

	if command -v show2.elf >/dev/null 2>&1 && [ -f "$PAK_DIR/res/blank.png" ]; then
		rm -f "$FIFO_PATH"
		show2.elf --mode=daemon --image="$PAK_DIR/res/blank.png" --text="$INIT_MSG" --progress=0 >/dev/null 2>&1 &
		SHOW2_PID=$!
		wait_i=0
		while [ ! -p "$FIFO_PATH" ] && [ "$wait_i" -lt 20 ]; do
			if ! kill -0 "$SHOW2_PID" 2>/dev/null; then
				SHOW2_PID=""
				break
			fi
			usleep 100000 2>/dev/null || sleep 1
			wait_i=$((wait_i + 1))
		done
	fi
	if [ -z "$SHOW2_PID" ] || ! kill -0 "$SHOW2_PID" 2>/dev/null; then
		SHOW2_PID=""
		minui-presenter --message "$INIT_MSG" --timeout -1 >/dev/null 2>&1 &
		PRESENTER_PID=$!
	fi

	: > "$SYNC_LOG"
	if [ "$SKIP_DISC" = "1" ]; then
		"$BIN" --badges "$BADGES" --skip-disc-systems "$@" --fifo "$FIFO_PATH" > "$SYNC_LOG" 2>&1
	else
		"$BIN" --badges "$BADGES" "$@" --fifo "$FIFO_PATH" > "$SYNC_LOG" 2>&1
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
}

system_menu() {
	TAB="$(printf '\t')"
	while true; do
		if [ ! -s "$SYS_LIST_TSV" ] || [ "$SYS_LIST_CACHED_SKIP_DISC" != "$SKIP_DISC" ]; then
			start_busy "Scanning ROM systems..."
			if [ "$SKIP_DISC" = "1" ]; then
				"$BIN" --list-systems --skip-disc-systems > "$SYS_LIST_TSV" 2>/dev/null
			else
				"$BIN" --list-systems > "$SYS_LIST_TSV" 2>/dev/null
			fi
			LIST_RC=$?
			stop_busy
			if [ "$LIST_RC" -ne 0 ]; then
				rm -f "$SYS_LIST_TSV"
				SYS_LIST_CACHED_SKIP_DISC=""
				minui-presenter --message "Failed to scan ROM systems" --timeout 0 --confirm-show --confirm-text "OK"
				return
			fi

			if [ ! -s "$SYS_LIST_TSV" ]; then
				SYS_LIST_CACHED_SKIP_DISC=""
				minui-presenter --message "No supported ROM systems found" --timeout 0 --confirm-show --confirm-text "OK"
				return
			fi

			printf '[' > "$SYS_MENU_FILE"
			first_sys=1
			while IFS="$TAB" read -r s_folder s_tag s_count; do
				[ -n "$s_folder" ] || continue
				esc_folder="$(printf '%s' "$s_folder" | sed 's/\\/\\\\/g; s/"/\\"/g')"
				[ "$first_sys" -eq 0 ] && printf ',' >> "$SYS_MENU_FILE"
				printf '"%s (%s ROMs)"' "$esc_folder" "$s_count" >> "$SYS_MENU_FILE"
				first_sys=0
			done < "$SYS_LIST_TSV"
			printf ']\n' >> "$SYS_MENU_FILE"
			SYS_LIST_CACHED_SKIP_DISC="$SKIP_DISC"
		fi

		rm -f "$SYS_OUT_FILE"
		minui-list --disable-auto-sleep --file "$SYS_MENU_FILE" --format json --title "Sync by System" --confirm-text "SELECT" --write-location "$SYS_OUT_FILE" >/dev/null
		RC=$?
		if [ "$RC" -ne 0 ] || [ ! -f "$SYS_OUT_FILE" ]; then
			return
		fi

		SYS_CHOICE="$(cat "$SYS_OUT_FILE")"
		SYS_FOLDER=""
		SYS_TAG=""
		while IFS="$TAB" read -r s_folder s_tag s_count; do
			if [ "${s_folder} (${s_count} ROMs)" = "$SYS_CHOICE" ]; then
				SYS_FOLDER="$s_folder"
				SYS_TAG="$s_tag"
				break
			fi
		done < "$SYS_LIST_TSV"

		if [ -z "$SYS_FOLDER" ]; then
			continue
		fi

		while true; do
			cat << EOF > "$ACT_MENU_FILE"
["Sync Uncached in $SYS_TAG","Sync All in $SYS_TAG","Select Games Manually...","Back"]
EOF
			rm -f "$ACT_OUT_FILE"
			minui-list --disable-auto-sleep --file "$ACT_MENU_FILE" --format json --title "$SYS_FOLDER" --confirm-text "SELECT" --write-location "$ACT_OUT_FILE" >/dev/null
			RC=$?
			if [ "$RC" -ne 0 ] || [ ! -f "$ACT_OUT_FILE" ]; then
				break
			fi

			ACT_CHOICE="$(cat "$ACT_OUT_FILE")"
			case "$ACT_CHOICE" in
				"Sync Uncached in $SYS_TAG")
					run_sync_with_ui --system "$SYS_FOLDER"
					STATUS_DIRTY=1
					;;
				"Sync All in $SYS_TAG")
					run_sync_with_ui --system "$SYS_FOLDER" --force
					STATUS_DIRTY=1
					;;
				"Select Games Manually...")
					start_busy "Scanning $SYS_FOLDER..."
					"$BIN" --list-games-json "$SYS_FOLDER" > "$GAMES_JSON" 2>/dev/null
					GAMES_RC=$?
					stop_busy
					if [ "$GAMES_RC" -ne 0 ] || [ ! -s "$GAMES_JSON" ]; then
						minui-presenter --message "Failed to list games for $SYS_FOLDER" --timeout 0 --confirm-show --confirm-text "OK"
						continue
					fi
					rm -f "$GAMES_OUT"
					minui-list --disable-auto-sleep --file "$GAMES_JSON" --format json --item-key games --title "$SYS_FOLDER" --confirm-text "SYNC" --write-value state --write-location "$GAMES_OUT" >/dev/null
					RC=$?
					if [ "$RC" -eq 0 ] && [ -f "$GAMES_OUT" ]; then
						run_sync_with_ui --system "$SYS_FOLDER" --sync-json "$GAMES_OUT" --force
						STATUS_DIRTY=1
					fi
					;;
				"Back"|"")
					break
					;;
				*)
					break
					;;
			esac
		done
	done
}

settings_menu() {
	b_idx=0
	case "$BADGES" in
		unlocked) b_idx=1 ;;
		none)     b_idx=2 ;;
	esac
	d_idx=0
	[ "$SKIP_DISC" = "1" ] && d_idx=1

	cat << EOF > "$SETTINGS_JSON"
{"settings":[{"name":"Download Badges","options":["All","Unlocked Only","None"],"selected":$b_idx},{"name":"Skip Disc Systems","options":["Off","On"],"selected":$d_idx}]}
EOF

	rm -f "$SETTINGS_OUT"
	minui-list --disable-auto-sleep --file "$SETTINGS_JSON" --format json --item-key settings --title "CheevoScraper Settings" --confirm-text "SAVE" --write-value state --write-location "$SETTINGS_OUT" >/dev/null
	RC=$?
	if [ "$RC" -eq 0 ] && [ -f "$SETTINGS_OUT" ]; then
		PARSED="$("$BIN" --parse-settings-json "$SETTINGS_OUT" 2>/dev/null)"
		if [ $? -eq 0 ] && [ -n "$PARSED" ]; then
			while IFS='=' read -r p_key p_val; do
				case "$p_key" in
					BADGES)
						case "$p_val" in
							all|unlocked|none) BADGES="$p_val" ;;
						esac
						;;
					SKIP_DISC)
						case "$p_val" in
							0|1) SKIP_DISC="$p_val" ;;
						esac
						;;
				esac
			done << EOF
$PARSED
EOF
			save_settings
		fi
	fi
}

cache_tools_menu() {
	TOOLS_MENU_FILE="/tmp/${PAK_NAME}-tools-menu.json"
	TOOLS_OUT_FILE="/tmp/${PAK_NAME}-tools-out.txt"
	cat << 'EOF' > "$TOOLS_MENU_FILE"
["Force Rebuild Cache","Re-check Unrecognized","Cache Info","Back"]
EOF
	while true; do
		rm -f "$TOOLS_OUT_FILE"
		minui-list --disable-auto-sleep --file "$TOOLS_MENU_FILE" --format json --title "Cache Tools" --confirm-text "SELECT" --write-location "$TOOLS_OUT_FILE" >/dev/null
		RC=$?
		if [ "$RC" -ne 0 ] || [ ! -f "$TOOLS_OUT_FILE" ]; then
			break
		fi
		T_CHOICE="$(cat "$TOOLS_OUT_FILE")"
		case "$T_CHOICE" in
			"Force Rebuild Cache")
				if confirm_action "Re-hash and re-fetch ALL games?\nThis may take a long time."; then
					run_sync_with_ui --force
					STATUS_DIRTY=1
				fi
				;;
			"Re-check Unrecognized")
				OUT="$("$BIN" --clear-unrecognized 2>&1)"
				STATUS_DIRTY=1
				minui-presenter --message "$OUT" --timeout 0 --confirm-show --confirm-text "OK"
				;;
			"Cache Info")
				start_busy "Calculating cache stats..."
				INFO="$("$BIN" --cache-info)"
				stop_busy
				minui-presenter --message "$INFO" --timeout 0 --confirm-show --confirm-text "OK"
				;;
			*)
				break
				;;
		esac
	done
	rm -f "$TOOLS_MENU_FILE" "$TOOLS_OUT_FILE"
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

load_settings

cat << 'EOF' > "$MENU_FILE"
["Sync by System...","Sync New Games","Refresh Sessions","Cache Tools...","Settings","Exit"]
EOF

while true; do
	if [ "$STATUS_DIRTY" -eq 1 ]; then
		STATUS_LINE="$("$BIN" --status)" || break
		STATUS_DIRTY=0
	fi
	rm -f "$OUT_FILE"
	minui-list --disable-auto-sleep --file "$MENU_FILE" --format json --title "$STATUS_LINE" --confirm-text "SELECT" --write-location "$OUT_FILE" >/dev/null
	RC=$?
	if [ "$RC" -ne 0 ] || [ ! -f "$OUT_FILE" ]; then
		echo "minui-list main menu exited with code $RC" >&2
		break
	fi

	CHOICE="$(cat "$OUT_FILE")"
	case "$CHOICE" in
		"Sync by System...")
			system_menu
			;;
		"Sync New Games")
			if confirm_action "Scan all ROM folders and cache new games?\nThis may take a while."; then
				run_sync_with_ui
				STATUS_DIRTY=1
			fi
			;;
		"Refresh Sessions")
			if confirm_action "Refresh session unlocks for all cached games?"; then
				run_sync_with_ui --refresh-sessions
				STATUS_DIRTY=1
			fi
			;;
		"Cache Tools...")
			cache_tools_menu
			;;
		"Settings")
			settings_menu
			;;
		"Exit"|"")
			break
			;;
		*)
			break
			;;
	esac
done

exit 0
