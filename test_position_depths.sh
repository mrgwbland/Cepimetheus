#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
POSITIONS_FILE="$SCRIPT_DIR/test_positions.txt"
DEFAULT_DEPTH_MIN=1
DEFAULT_DEPTH_MAX=6

FEN=""

print_saved_positions() {
    if [[ ! -f "$POSITIONS_FILE" ]]; then
        return
    fi

    echo "Saved test positions from $POSITIONS_FILE:"
    local line idx=1
    while IFS= read -r line; do
        [[ -z "$line" || "$line" =~ ^[[:space:]]*# ]] && continue
        local name="${line%%|*}"
        local fen="${line#*|}"
        if [[ -n "$name" && -n "$fen" && "$name" != "$fen" ]]; then
            echo "  [$idx] $name"
            echo "      $fen"
            ((idx++))
        fi
    done < "$POSITIONS_FILE"
    echo
}

resolve_saved_fen_by_number() {
    local wanted="$1"
    local line idx=1

    while IFS= read -r line; do
        [[ -z "$line" || "$line" =~ ^[[:space:]]*# ]] && continue
        local name="${line%%|*}"
        local fen="${line#*|}"
        if [[ -n "$name" && -n "$fen" && "$name" != "$fen" ]]; then
            if [[ "$idx" == "$wanted" ]]; then
                printf "%s" "$fen"
                return 0
            fi
            ((idx++))
        fi
    done < "$POSITIONS_FILE"

    return 1
}

prompt_for_fen() {
    print_saved_positions

    read -r -p "Choose saved position number or press Enter to type a FEN: " selection

    if [[ -n "${selection:-}" ]]; then
        if FEN="$(resolve_saved_fen_by_number "$selection")"; then
            echo "Using saved FEN: $FEN"
            return
        fi
        echo "Invalid selection; falling back to manual FEN entry."
    fi

    read -r -p "Enter FEN: " FEN
    if [[ -z "$FEN" ]]; then
        echo "No FEN provided." >&2
        exit 1
    fi
}

prompt_for_depth_range() {
    local input_min input_max
    read -r -p "Start depth [$DEFAULT_DEPTH_MIN]: " input_min
    read -r -p "End depth [$DEFAULT_DEPTH_MAX]: " input_max

    DEPTH_MIN="${input_min:-$DEFAULT_DEPTH_MIN}"
    DEPTH_MAX="${input_max:-$DEFAULT_DEPTH_MAX}"

    if ! [[ "$DEPTH_MIN" =~ ^[0-9]+$ && "$DEPTH_MAX" =~ ^[0-9]+$ ]]; then
        echo "Depth values must be non-negative integers." >&2
        exit 1
    fi

    if (( DEPTH_MIN > DEPTH_MAX )); then
        echo "Start depth cannot be greater than end depth." >&2
        exit 1
    fi
}

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

prompt_for_fen
prompt_for_depth_range

for ((d = DEPTH_MIN; d <= DEPTH_MAX; ++d)); do
    echo "=== depth $d ==="
    run_depth "$d"
    echo
done
