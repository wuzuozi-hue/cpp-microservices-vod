#!/usr/bin/env bash
set -euo pipefail

# Development supervisor: keep services in the foreground and stop only the
# child processes started by this invocation. Infrastructure is external.
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
bin_dir=${VOD_BIN_DIR:-"$root/build"}
log_dir=${VOD_LOG_DIR:-"$root/run-logs"}
: "${VOD_DB_USER:?Set VOD_DB_USER}"
: "${VOD_DB_NAME:?Set VOD_DB_NAME}"
: "${VOD_FDFS_TRACKER:?Set VOD_FDFS_TRACKER to host:port}"
db_host=${VOD_DB_HOST:-127.0.0.1}
db_port=${VOD_DB_PORT:-3306}
etcd=${VOD_ETCD_ADDRESS:-http://127.0.0.1:2379}
http_port=${VOD_HTTP_PORT:-18082}
instance="local-$$"
mkdir -p -- "$log_dir"
for binary in vod_user_server vod_file_server vod_video_server vod_transcode_worker vod_gateway; do
    if [[ ! -x "$bin_dir/$binary" ]]; then printf 'Missing executable: %s\n' "$bin_dir/$binary" >&2; exit 1; fi
done

pids=()
cleanup() {
    trap - EXIT INT TERM
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    for pid in "${pids[@]}"; do wait "$pid" 2>/dev/null || true; done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
start() {
    local name=$1
    shift
    "$bin_dir/$name" "$@" >>"$log_dir/$name.log" 2>&1 &
    pids+=("$!")
}
db_flags=("--db_host=$db_host" "--db_port=$db_port" "--db_user=$VOD_DB_USER" "--db_name=$VOD_DB_NAME")
start vod_user_server "--instance_id=$instance" "--etcd_address=$etcd" "${db_flags[@]}"
start vod_file_server "--instance_id=$instance" "--etcd_address=$etcd" "--tracker=$VOD_FDFS_TRACKER" "${db_flags[@]}"
start vod_video_server "--instance_id=$instance" "--etcd_address=$etcd" "${db_flags[@]}"
start vod_transcode_worker "--tracker=$VOD_FDFS_TRACKER" "${db_flags[@]}"
start vod_gateway "--etcd_address=$etcd" --http_host=127.0.0.1 "--http_port=$http_port"
printf 'Services launched; logs: %s\nGateway: http://127.0.0.1:%s\nPress Ctrl-C to stop these processes.\n' "$log_dir" "$http_port"
if wait -n "${pids[@]}"; then status=0; else status=$?; fi
printf 'A service exited (status %s); stopping the remaining services.\n' "$status" >&2
exit 1
