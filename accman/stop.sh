#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
var_dir="$root_dir/var"
pid_file="$var_dir/server.pid"

if [[ ! -f "$pid_file" ]]; then
    echo "Not running (no pid file)."
    exit 0
fi

pid="$(cat "$pid_file")"
if kill -0 "$pid" 2>/dev/null; then
    kill "$pid"
    sleep 0.5
    if kill -0 "$pid" 2>/dev/null; then
        echo "Process $pid did not exit; send SIGKILL with: kill -9 $pid"
        exit 1
    fi
    echo "Stopped (pid $pid)."
else
    echo "Process not running (pid $pid)."
fi

rm -f "$pid_file"
