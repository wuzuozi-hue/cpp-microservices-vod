#!/usr/bin/env bash
set -euo pipefail
# Local foreground launch. Credentials are read without echo and are never
# written to this script or to the shell history.
if [[ ! -t 0 ]]; then echo 'Run this script in an interactive terminal.' >&2; exit 1; fi
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
python3 "$root/scripts/start-db.py"
python3 "$root/scripts/start-infrastructure.py"
set -a
source "$root/runtime/mysql/credentials.env"
source "$root/runtime/infrastructure/endpoints.env"
set +a
db_base=$(cat "$root/runtime/mysql/basedir.txt")
export VOD_MYSQL_CLIENT="$db_base/bin/mariadb"
export LD_LIBRARY_PATH="$db_base/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
bash "$root/scripts/init-db.sh"
read -r -p 'QQ 发件邮箱: ' VOD_SMTP_USER
read -r -s -p 'QQ 邮箱 SMTP 授权码（输入时不显示）: ' VOD_SMTP_PASSWORD
printf '\n'
if [[ -z $VOD_SMTP_USER || -z $VOD_SMTP_PASSWORD ]]; then echo '邮箱和授权码不能为空。' >&2; exit 2; fi
export VOD_SMTP_USER VOD_SMTP_PASSWORD
export VOD_SMTP_FROM="$VOD_SMTP_USER"
export VOD_SMTP_URL='smtps://smtp.qq.com:465'
export VOD_SMTP_LOGIN_OPTIONS='AUTH=LOGIN'
export VOD_CODE_SECRET="$(openssl rand -hex 32)"
exec bash "$root/scripts/run-local.sh"
