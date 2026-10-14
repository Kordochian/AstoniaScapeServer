#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
var_dir="$root_dir/var"
pid_file="$var_dir/server.pid"
log_file="$var_dir/server.log"

mkdir -p "$var_dir"

if [[ -f "$pid_file" ]]; then
    pid="$(cat "$pid_file")"
    if kill -0 "$pid" 2>/dev/null; then
        echo "Already running (pid $pid)."
        exit 0
    fi
fi

php -S 0.0.0.0:8088 -t "$root_dir/public" >"$log_file" 2>&1 &
echo $! > "$pid_file"

echo "Started on http://0.0.0.0:8088 (pid $(cat "$pid_file"))."
