#!/usr/bin/env bash
set -euo pipefail
# One-time bootstrap for a dedicated application database. The account must
# already have verified its email through the normal login flow.
: "${VOD_DB_USER:?Set VOD_DB_USER}"
: "${VOD_DB_NAME:?Set VOD_DB_NAME}"
: "${VOD_BOOTSTRAP_EMAIL:?Set VOD_BOOTSTRAP_EMAIL}"
if [[ ! $VOD_DB_NAME =~ ^[A-Za-z0-9_]+$ || ! $VOD_BOOTSTRAP_EMAIL =~ ^[A-Za-z0-9._+@-]+$ || $VOD_BOOTSTRAP_EMAIL != *@* ]]; then
    echo 'Invalid database name or email.' >&2; exit 2
fi
client=${VOD_MYSQL_CLIENT:-mysql}
if ! command -v "$client" >/dev/null; then echo 'MySQL client not found.' >&2; exit 1; fi
export MYSQL_PWD=${VOD_DB_PASSWORD-}
sql="SELECT GET_LOCK(CONCAT('vod-bootstrap:', DATABASE()), 10) INTO @locked;
UPDATE vod_users SET role_type=1 WHERE email='$VOD_BOOTSTRAP_EMAIL' AND user_status=1 AND @locked=1
AND NOT EXISTS (SELECT 1 FROM (SELECT user_id FROM vod_users WHERE role_type=1 LIMIT 1) existing_admin);
SELECT ROW_COUNT();
DO RELEASE_LOCK(CONCAT('vod-bootstrap:', DATABASE()));"
result=$("$client" --no-defaults --protocol=tcp -h "${VOD_DB_HOST:-127.0.0.1}" -P "${VOD_DB_PORT:-3306}" -u "$VOD_DB_USER" -D "$VOD_DB_NAME" --batch --skip-column-names -e "$sql")
if [[ $result != 1 ]]; then echo 'No account was changed: verify the registered email and that no super administrator already exists.' >&2; exit 1; fi
echo 'Super administrator initialized.'
