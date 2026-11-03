#!/bin/bash
#
# Double-click to build the macOS game: pick the game dump folder in a
# normal Finder dialog and watch the progress here.

cd -- "$(dirname -- "$0")" || exit 1
ROOT="$PWD"

dialog() { osascript -e "$1" 2>/dev/null; }
alert() { dialog "display dialog \"$1\" buttons {\"OK\"} default button 1 with title \"Splatoon Setup\" with icon ${2:-note}" >/dev/null; }

clear
printf '============================================================\n'
printf '   Splatoon 1 PC Port - macOS\n'
printf '============================================================\n\n'

# Game dump folder.
dump=""
while [ -z "$dump" ]; do
    dump="$(dialog 'POSIX path of (choose folder with prompt "Select your Splatoon dump folder (the one that contains code, content and meta):" default location (path to downloads folder))')"
    if [ -z "$dump" ]; then printf 'Cancelled.\n'; exit 0; fi
    dump="${dump%/}"
    if [ ! -f "$dump/code/Gambit.rpx" ] || [ ! -d "$dump/content" ] || [ ! -d "$dump/meta" ]; then
        alert "That folder does not look like a Splatoon dump. It must contain the folders code, content and meta." caution
        dump=""
    fi
done
mkdir -p "$ROOT/build"

printf 'Game folder: %s\n' "$dump"
printf 'This window shows the progress. The first build takes about 5-10 minutes.\n'
start=$(date +%s)

# Live progress: the setup's step lines, plus a running counter while the
# game compiles (Ninja prints [done/total] for each file).
"$ROOT/setup_macos.sh" --non-interactive --dump "$dump" 2>&1 | tee "$ROOT/build/setup-menu.log" | while IFS= read -r line; do
    if [[ "$line" =~ ^\[([0-9]+)/([0-9]+)\]\ (Building|Linking)\ (CXX|C)\  ]]; then
        printf '\r      Compiling: %s / %s files (%d%%)   ' "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" \
            $(( ${BASH_REMATCH[1]} * 100 / ${BASH_REMATCH[2]} ))
    elif [[ "$line" =~ ^\[[0-9]/9\] ]] || [[ "$line" =~ (SETUP\ FAILED|error:|Setup\ complete|Play:) ]]; then
        printf '\n%s\n' "$line"
    fi
done
status=${PIPESTATUS[0]}
elapsed=$(( $(date +%s) - start ))
printf '\n'

if [ "$status" -eq 0 ]; then
    osascript -e "display notification \"Finished in $((elapsed / 60)) min $((elapsed % 60)) s\" with title \"Splatoon is ready\"" 2>/dev/null
    play="$(dialog "button returned of (display dialog \"Splatoon is ready (built in $((elapsed / 60)) min $((elapsed % 60)) s).\" buttons {\"Close\", \"Play now\"} default button 2 with title \"Splatoon Setup\")")"
    [ "$play" = "Play now" ] && open "$ROOT/Game/Splatoon.app"
else
    printf 'The build failed. The full log is in build/setup-menu.log\n'
    alert "The build failed. Look at this window for the error, or run it again." stop
fi
