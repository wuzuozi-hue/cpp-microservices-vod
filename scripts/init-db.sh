#!/usr/bin/env bash
set -euo pipefail
: "${VOD_DB_USER:?Set VOD_DB_USER}"
: "${VOD_DB_NAME:?Set VOD_DB_NAME to an existing dedicated database}"
if [[ ! $VOD_DB_NAME =~ ^[A-Za-z0-9_]+$ ]]; then echo 'Invalid database name.' >&2; exit 2; fi
client=${VOD_MYSQL_CLIENT:-mysql}
if ! command -v "$client" >/dev/null; then echo 'MySQL client not found.' >&2; exit 1; fi
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
export MYSQL_PWD=${VOD_DB_PASSWORD-}
for sql in 001_sessions.sql 002_videos.sql 003_accounts.sql 004_files.sql; do
    "$client" --no-defaults --protocol=tcp -h "${VOD_DB_HOST:-127.0.0.1}" -P "${VOD_DB_PORT:-3306}" -u "$VOD_DB_USER" -D "$VOD_DB_NAME" < "$root/sql/$sql"
done
echo 'Database tables initialized.'
