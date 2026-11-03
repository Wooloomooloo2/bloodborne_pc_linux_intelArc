#!/usr/bin/env bash
# abench.sh NAME UPSCALER PRESET [ENV=VALUE...]: one unattended benchmark run on the Arc B390.
# A fresh copy of the save, Continue at the title, wait for the level, settle, then measure HOLD s
# standing still: per-frame CSV + GPU clock, throttle reasons and package temperature each second.
# Finds the game by its own save folder (BB_USER_DIR), not by process name: a shader cache rebuild
# runs in a short-lived child with the same name, which tools/restart.sh mistook for the game.
set -u
REPO=${REPO:-$(cd "$(dirname "$0")/../.." && pwd)}
B=$(cd -- "$(dirname -- "$0")" && pwd)
RESULTS=${RESULTS:-$B/results_ab}
HOLD=${HOLD:-60}
SETTLE=${SETTLE:-15}
GT=/sys/class/drm/card1/device/tile0/gt0/freq0
TEMP=/sys/class/hwmon/hwmon11/temp1_input
mkdir -p "$RESULTS"
name=$1 up=$2 preset=$3; shift 3

if pgrep -x bb-probe > /dev/null; then
    echo "abench: a game is already running; not starting" >&2
    exit 1
fi
ini=$B/$name.ini user=$B/user_$name
sed -e '/^upscaler=/d' -e '/^preset=/d' "$HOME/Games/bbport-dev/bbport.ini" > "$ini"
printf 'upscaler=%s\npreset=%s\n' "$up" "$preset" >> "$ini"
rm -rf "$user"; cp -a "$HOME/Games/bbport-dev/user" "$user"
log=$RESULTS/$name.log csv=$RESULTS/$name.frames.csv sensors=$RESULTS/$name.sensors.csv
rm -f "$csv"
mkdir -p "$REPO/out"; : > "$REPO/out/pad"
rm -rf "$REPO/out/present"; mkdir -p "$REPO/out/present"

(cd "$REPO" && env BB_GAME_DIR=$HOME/Games/CUSA03173 BB_USER_DIR="$user" BB_CONFIG="$ini" \
    BB_FPS=uncap BB_FPS_LIMIT=0 BB_FULLSCREEN=0 BB_SKIP_NETWORK_CHOICE=1 \
    BB_PAD_FILE="$REPO/out/pad" BB_FRAME_STATS=1 BB_FRAME_LOG="$csv" \
    BB_PRESENT_DUMP_TRIGGER="$REPO/out/present_trigger" BB_DUMP_DIR="$REPO/out/present" \
    SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1 "$@" \
    setsid stdbuf -oL -eL bash -c 'if [[ -n ${LOG_FILTER:-} ]]; then bash run.sh 2>&1 | grep --line-buffered -aE "$LOG_FILTER"; else bash run.sh 2>&1; fi' > "$log" < /dev/null &)

game_pid() { # the bb-probe whose BB_USER_DIR is this run's, the oldest one
    local p
    for p in $(pgrep -o -x bb-probe) $(pgrep -x bb-probe); do
        if tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx "BB_USER_DIR=$user"; then
            echo "$p"; return 0
        fi
    done
    return 1
}
stop_game() {
    local p
    for p in $(pgrep -x bb-probe); do
        tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx "BB_USER_DIR=$user" && kill "$p"
    done
    sleep 3
    for p in $(pgrep -x bb-probe); do
        tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -qx "BB_USER_DIR=$user" && kill -9 "$p"
    done
}
press() { echo "$1" > "$REPO/out/pad"; sleep 0.3; : > "$REPO/out/pad"; }

# Title: the pad is open and frame stats report a few windows.
ok=0
for i in $(seq 1 240); do
    sleep 1
    if ! game_pid > /dev/null && (( i > 30 )); then break; fi
    if [[ $(grep -c '^Frame stats' "$log") -ge 3 ]] && grep -q 'pad opened' "$log"; then ok=1; break; fi
done
if (( ! ok )); then echo "abench $name: no title screen (see $log)"; stop_game; exit 1; fi
sleep 6
press cross
# The level: a stats window with more than 300 draws per frame.
ok=0
for i in $(seq 1 120); do
    sleep 2
    last=$(grep '^Frame stats' "$log" | tail -1 | sed -E 's/.* ([0-9]+) draws\/frame.*/\1/')
    [[ -n $last && $last =~ ^[0-9]+$ && $last -gt 300 ]] && { ok=1; break; }
done
if (( ! ok )); then echo "abench $name: never reached the level (see $log)"; stop_game; exit 1; fi
sleep "$SETTLE"

start_line=$(wc -l < "$csv")
echo "t,act_mhz,cur_mhz,temp_c,throttle" > "$sensors"
for i in $(seq 1 "$HOLD"); do
    if (( i == 20 )); then : > "$REPO/out/present_trigger"; fi
    printf '%s,%s,%s,%s,%s\n' "$i" "$(cat $GT/act_freq)" "$(cat $GT/cur_freq)" \
        "$(( $(cat $TEMP) / 1000 ))" "$(cat $GT/throttle/reasons | tr ' ,' '+;')" >> "$sensors"
    sleep 1
done
end_line=$(wc -l < "$csv")
stop_game
shot=$(ls -t "$REPO/out/present" 2>/dev/null | head -1)
[[ -n $shot ]] && cp "$REPO/out/present/$shot" "$RESULTS/$name.${shot##*.}"
echo "$start_line $end_line" > "$RESULTS/$name.range"
python3 -I "$B/summarize.py" "$RESULTS" "$name"
[[ ${KEEP_USER:-0} == 1 ]] || rm -rf "$user"
