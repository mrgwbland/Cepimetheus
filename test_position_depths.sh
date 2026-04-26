#!/usr/bin/env bash
set -euo pipefail

# Nxe5, Qf4 good moves, everything else blunder
FEN='3r2rk/pp3p1p/2p1pq2/2P1P2R/2n5/3P4/P3Q1B1/1K1R4 b - - 0 34'

run_depth() {
    local depth="$1"

    coproc ENGINE { ./Cepimetheus; }
    local engine_pid="$ENGINE_PID"

    local in_fd="${ENGINE[1]}"
    local out_fd="${ENGINE[0]}"

    printf "uci\nisready\nposition fen %s\ngo depth %d\n" "$FEN" "$depth" >&"$in_fd"

    local line
    while IFS= read -r line <&"$out_fd"; do
        if [[ "$line" =~ ^(info\ depth|bestmove|readyok|uciok) ]] && [[ "$line" != *currmove* ]]; then
            echo "$line"
        fi

        if [[ "$line" == bestmove* ]]; then
            break
        fi
    done

    printf "quit\n" >&"$in_fd" || true

    exec {in_fd}>&-
    exec {out_fd}<&-

    wait "$engine_pid" 2>/dev/null || true
}

for d in 1 2 3 4 5 6; do
    echo "=== depth $d ==="
    run_depth "$d"
    echo
done
