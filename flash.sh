#!/usr/bin/env bash
# Build and flash esp32c5-zigbee-hub, provisioning it with this machine's Wi-Fi.
#
#   ./flash.sh                         build + flash + provision, wait for the board
#   ./flash.sh --prov-only             only rewrite credentials (no build, Zigbee untouched)
#   ./flash.sh --backup-ssid X --backup-psk Y    add a backup network (e.g. phone hotspot)
#   ./flash.sh --ssid X --psk Y        provision a network other than the current one
#   ./flash.sh --hostname NAME         default: zigbee-hub
#   ./flash.sh --ts-authkey KEY        Tailscale auth key (Phase 4)
#   ./flash.sh --monitor               keep showing the serial log afterwards
#   ./flash.sh --port /dev/ttyACM0     default: $ESPPORT from env.sh
#   ./flash.sh --ota [--host H]        build and update over the network (no USB);
#                                      default host: <hostname>.local
#
# Options you pass are remembered in .esp/secrets/prov.env (gitignored, 0600), so
# the next run keeps your backup network, hostname and Tailscale key.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

die()  { printf '\e[31m✗ %s\e[0m\n' "$*" >&2; exit 1; }
warn() { printf '\e[33m! %s\e[0m\n' "$*" >&2; }
ok()   { printf '\e[32m✓\e[0m %s\n' "$*"; }
info() { printf '  %s\n' "$*"; }

# ---------------------------------------------------------------- arguments
PROV_ONLY=0 MONITOR=0 PORT="" OTA=0 OTA_HOST=""
ARG_SSID="" ARG_PSK="" ARG_BSSID_SET=0 ARG_BSSID="" ARG_BPSK=""
ARG_HOST="" ARG_TSKEY=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --prov-only)   PROV_ONLY=1 ;;
    --ota)         OTA=1 ;;
    --host)        OTA_HOST="$2"; shift ;;
    --monitor)     MONITOR=1 ;;
    --port)        PORT="$2"; shift ;;
    --ssid)        ARG_SSID="$2"; shift ;;
    --psk)         ARG_PSK="$2"; shift ;;
    --backup-ssid) ARG_BSSID_SET=1; ARG_BSSID="$2"; shift ;;
    --backup-psk)  ARG_BPSK="$2"; shift ;;
    --no-backup)   ARG_BSSID_SET=1; ARG_BSSID=""; ARG_BPSK="" ;;
    --hostname)    ARG_HOST="$2"; shift ;;
    --ts-authkey)  ARG_TSKEY="$2"; shift ;;
    -h|--help)     sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (see --help)" ;;
  esac
  shift
done

# ---------------------------------------------------------------- toolchain
[[ -f .esp/esp-idf/export.sh ]] || die "toolchain missing in .esp/ (see CLAUDE.md, Toolchain)"
# ESP-IDF's export.sh isn't written for `set -eu`; relax it just for sourcing.
set +eu
# shellcheck disable=SC1091
source ./env.sh >/dev/null 2>&1; ENV_RC=$?
set -eu
[[ $ENV_RC == 0 ]] || die "source ./env.sh failed"
command -v idf.py >/dev/null || die "idf.py not on PATH after env.sh"
PY="$IDF_PYTHON_ENV_PATH/bin/python"

SECRETS="$ROOT/.esp/secrets"
mkdir -p "$SECRETS"; chmod 700 "$SECRETS"
STATE="$SECRETS/prov.env"
SAVED_BACKUP_SSID="" SAVED_BACKUP_PSK="" SAVED_HOSTNAME="" SAVED_TS_AUTHKEY=""
# shellcheck disable=SC1090
[[ -f "$STATE" ]] && source "$STATE"

build() {
  info "building firmware…"
  idf.py build >"$ROOT/.esp/last-build.log" 2>&1 || { tail -30 "$ROOT/.esp/last-build.log"; die "build failed (full log: .esp/last-build.log)"; }
  ok "firmware built ($(stat -c %s build/esp32c5-zigbee-hub.bin | numfmt --to=iec))"
}

# ---------------------------------------------------------------- OTA (network update)
if [[ "$OTA" == 1 ]]; then
  command -v curl >/dev/null || die "curl not found"
  HOST="${OTA_HOST:-${SAVED_HOSTNAME:-zigbee-hub}.local}"
  [[ -s "$SECRETS/admin_password" ]] || die "no admin password in .esp/secrets (flash over USB once first)"
  build
  BIN=build/esp32c5-zigbee-hub.bin
  NEW_ID=$("$PY" -I -c 'import sys,hashlib; d=open(sys.argv[1],"rb").read(); print(d[32+144:32+144+8].hex())' "$BIN")
  JAR=$(mktemp); trap 'rm -f "$JAR"' EXIT
  # Password goes through stdin, never argv.
  printf '{"password":"%s"}' "$(<"$SECRETS/admin_password")" |
    curl -sf --max-time 10 -c "$JAR" -H 'X-Hub: 1' -H 'Content-Type: application/json' \
      --data @- "http://$HOST/api/login" >/dev/null || die "login to $HOST failed (is it online? right password?)"
  OLD_ID=$(curl -sf --max-time 5 -b "$JAR" "http://$HOST/api/status" | "$PY" -I -c 'import sys,json; print(json.load(sys.stdin)["firmware"]["build_id"])')
  [[ "$OLD_ID" == "$NEW_ID" ]] && warn "the hub already runs this exact build ($NEW_ID); installing anyway"
  info "uploading to $HOST…"
  T0=$(date +%s%N)
  RESP=$(curl -s --max-time 120 -b "$JAR" -H 'X-Hub: 1' -H 'Content-Type: application/octet-stream' \
           --data-binary @"$BIN" -w '\n%{http_code}' "http://$HOST/api/ota") || die "upload failed"
  CODE=${RESP##*$'\n'}
  [[ "$CODE" == 200 ]] || die "hub refused the update: ${RESP%$'\n'*}"
  ok "uploaded in $(( ($(date +%s%N) - T0) / 1000000 )) ms; the hub is restarting"
  info "waiting for it to come back (up to 3 min; a failed update rolls back by itself)…"
  for _ in $(seq 1 90); do
    sleep 2
    ST=$(curl -sf --max-time 3 -b "$JAR" "http://$HOST/api/status" 2>/dev/null) || continue
    read -r RUN_ID RESULT TRIAL TO_ID < <("$PY" -I -c 'import sys,json; f=json.load(sys.stdin)["firmware"]; u=f["last_update"]; print(f["build_id"], u["result"], f["on_trial"], u.get("to") or "-")' <<<"$ST")
    if [[ "$RUN_ID" == "$NEW_ID" && ( "$RESULT" == updated || "$RESULT" == trial ) ]]; then
      if [[ "$TRIAL" == True || "$RESULT" == trial ]]; then info "running the new build $NEW_ID, confirming…"; continue; fi
      ok "updated to $NEW_ID and confirmed ($(( ($(date +%s%N) - T0) / 1000000 )) ms total)"; exit 0
    fi
    [[ "$RESULT" == rolled_back && "$TO_ID" == "$NEW_ID" ]] && die "the new firmware failed (crash or health check); the hub rolled back to $RUN_ID"
  done
  die "the hub didn't come back within 3 minutes"
fi

PORT="${PORT:-$ESPPORT}"
[[ -e "$PORT" ]] || die "serial port $PORT not found (is the board plugged in?)"

# ---------------------------------------------------------------- primary Wi-Fi
command -v nmcli >/dev/null || [[ -n "$ARG_SSID" ]] || die "nmcli not found; pass --ssid/--psk"

SSID="$ARG_SSID" PSK="$ARG_PSK" AUTO_DETECTED=0
if [[ -z "$SSID" ]]; then
  CONN=$(nmcli -t -f DEVICE,TYPE,STATE,CONNECTION dev | awk -F: '$2=="wifi" && $3=="connected"{print $4; exit}')
  [[ -n "$CONN" ]] || die "this machine isn't on Wi-Fi; pass --ssid and --psk"
  SSID=$(nmcli -g 802-11-wireless.ssid connection show "$CONN")
  KEYMGMT=$(nmcli -g 802-11-wireless-security.key-mgmt connection show "$CONN" 2>/dev/null || true)
  case "$KEYMGMT" in
    wpa-psk|sae) ;;
    "") die "'$SSID' is an open network; the hub requires WPA2 or WPA3" ;;
    *)  die "'$SSID' uses '$KEYMGMT'; the hub supports WPA2/WPA3-Personal only" ;;
  esac
  PSK=$(nmcli -s -g 802-11-wireless-security.psk connection show "$CONN" 2>/dev/null || true)
  if [[ -z "$PSK" ]]; then
    info "NetworkManager withholds the password without root; asking sudo"
    PSK=$(sudo nmcli -s -g 802-11-wireless-security.psk connection show "$CONN")
  fi
  AUTO_DETECTED=1
fi
[[ -n "$PSK" ]] || die "no password for '$SSID'"

# 5 GHz check against what this machine can see right now.
check_5ghz() {  # $1 = ssid, $2 = 1 if it must be visible
  local ssid="$1" must="$2" rows best
  rows=$(nmcli -t -f FREQ,CHAN,SSID dev wifi list 2>/dev/null | awk -F: -v want="$ssid" '{
      f=$1; c=$2; s=substr($0, length($1)+length($2)+3); gsub(/\\:/, ":", s);
      sub(/ MHz/, "", f); if (s == want) print f, c }')
  if [[ -z "$rows" ]]; then
    [[ "$must" == 1 ]] && die "'$ssid' isn't visible from this machine"
    warn "'$ssid' isn't visible from here; can't verify it has a 5 GHz access point"
    return
  fi
  best=$(awk '$1 >= 5000 {print $2; exit}' <<<"$rows")
  if [[ -z "$best" ]]; then
    die "'$ssid' is only on 2.4 GHz here. The hub uses 5 GHz only (2.4 GHz is for Zigbee)."
  fi
  if (( best >= 52 && best <= 144 )); then
    warn "'$ssid' is on DFS channel $best. The C5 can't detect radar; channels 36-48 are safer."
    warn "Testing on DFS anyway is fine — the hub logs every disconnect so we can measure it."
  else
    ok "'$ssid' has a 5 GHz access point (channel $best)"
  fi
}
check_5ghz "$SSID" "$AUTO_DETECTED"

# ---------------------------------------------------------------- remembered options
if [[ "$ARG_BSSID_SET" == 1 ]]; then
  BACKUP_SSID="$ARG_BSSID" BACKUP_PSK="$ARG_BPSK"
  [[ -z "$BACKUP_SSID" || -n "$BACKUP_PSK" ]] || die "--backup-ssid needs --backup-psk"
else
  BACKUP_SSID="$SAVED_BACKUP_SSID" BACKUP_PSK="$SAVED_BACKUP_PSK"
fi
HOSTNAME_="${ARG_HOST:-${SAVED_HOSTNAME:-zigbee-hub}}"
[[ "$HOSTNAME_" =~ ^[a-z0-9]([a-z0-9-]{0,30}[a-z0-9])?$ ]] || die "hostname must be a-z, 0-9, '-' (max 32)"
TS_AUTHKEY="${ARG_TSKEY:-$SAVED_TS_AUTHKEY}"
[[ -n "$BACKUP_SSID" ]] && check_5ghz "$BACKUP_SSID" 0

umask 077
{
  printf 'SAVED_BACKUP_SSID=%q\n' "$BACKUP_SSID"
  printf 'SAVED_BACKUP_PSK=%q\n' "$BACKUP_PSK"
  printf 'SAVED_HOSTNAME=%q\n' "$HOSTNAME_"
  printf 'SAVED_TS_AUTHKEY=%q\n' "$TS_AUTHKEY"
} >"$STATE"

# Admin password: generated once, kept here, only its hash goes to the board.
PWFILE="$SECRETS/admin_password"
if [[ ! -s "$PWFILE" ]]; then
  "$PY" -I -c 'import secrets,string; a=string.ascii_lowercase+string.digits; print("-".join("".join(secrets.choice(a) for _ in range(4)) for _ in range(3)))' >"$PWFILE"
  NEW_PW=1
else
  NEW_PW=0
fi
ADMIN_PW=$(<"$PWFILE")

COUNTRY=$(iw reg get 2>/dev/null | awk '/^global/{getline; sub(/:$/, "", $2); print $2; exit}' || true)
[[ "$COUNTRY" =~ ^[A-Z]{2}$ ]] || COUNTRY=""

# ---------------------------------------------------------------- build
[[ "$PROV_ONLY" == 0 ]] && build

# ---------------------------------------------------------------- prov image
PROV_OFF=$(awk -F, '{gsub(/[ \t]/, "")} $1=="prov"{print $4}' partitions.csv)
PROV_SIZE=$(awk -F, '{gsub(/[ \t]/, "")} $1=="prov"{print $5}' partitions.csv)
[[ -n "$PROV_OFF" && -n "$PROV_SIZE" ]] || die "no 'prov' partition in partitions.csv"
PROV_BIN="$SECRETS/prov.bin"
PROV_SSID="$SSID" PROV_PSK="$PSK" PROV_SSID2="$BACKUP_SSID" PROV_PSK2="$BACKUP_PSK" \
PROV_HOSTNAME="$HOSTNAME_" PROV_COUNTRY="$COUNTRY" PROV_ADMIN_PW="$ADMIN_PW" \
PROV_TS_AUTHKEY="$TS_AUTHKEY" \
  "$PY" -I tools/mkprov.py "$IDF_PATH" "$PROV_BIN" "$((PROV_SIZE))" || die "mkprov failed"

# ---------------------------------------------------------------- flash
ESPTOOL=("$PY" -m esptool --chip esp32c5 -p "$PORT" -b 460800 --before default-reset --after hard-reset)
info "flashing $( [[ $PROV_ONLY == 1 ]] && echo credentials || echo 'firmware + credentials') via $PORT…"
if [[ "$PROV_ONLY" == 1 ]]; then
  "${ESPTOOL[@]}" write-flash "$PROV_OFF" "$PROV_BIN" >"$ROOT/.esp/last-flash.log" 2>&1
else
  (cd build && "${ESPTOOL[@]}" write-flash "@flash_args" "$PROV_OFF" "$PROV_BIN") >"$ROOT/.esp/last-flash.log" 2>&1
fi || { tail -20 "$ROOT/.esp/last-flash.log"; die "flash failed (full log: .esp/last-flash.log)"; }
ok "flashed"

# ---------------------------------------------------------------- wait for the board
stty -F "$PORT" 115200 raw -echo
info "waiting for the board to join Wi-Fi (up to 90 s)…"
# awk reads the port itself and does all the cleanup. A pipeline here cost
# minutes: a filter (tr) block-buffered the HUB_READY line, and a `cat` feeding
# awk only noticed awk had exited on its next write, ~30 s later.
READY=$(timeout 90 awk '
  { gsub(/\r/, ""); gsub(/\033\[[0-9;]*m/, "") }
  / wifi5g: / { line = $0; sub(/^.*wifi5g: /, "", line)
                if (line !~ /^HUB_READY/) { print "    " line > "/dev/stderr"; fflush("/dev/stderr") } }
  /HUB_READY/ { sub(/^.*HUB_READY /, ""); print; exit }' <"$PORT" || true)

[[ -n "$READY" ]] || die "board didn't report an IP within 90 s (see log above; run with --monitor)"
kv() { sed -n "s/.*\b$1=\([^ ]*\).*/\1/p" <<<"$READY"; }
IP=$(kv ip) CH=$(kv ch) RSSI=$(kv rssi) NET=$(kv net) MS=$(kv boot_ms)
NETNAME="$SSID"; [[ "$NET" == backup ]] && NETNAME="$BACKUP_SSID"
DFS=""; (( CH >= 52 && CH <= 144 )) && DFS=" (DFS)"

echo
ok "Connected to $NETNAME on 5 GHz, channel $CH$DFS, $RSSI dBm, $((MS / 1000)).$(( (MS % 1000) / 100 )) s after boot"
echo
echo "  Board:    $IP   ($HOSTNAME_.local)"
echo "  Password: $ADMIN_PW$( [[ $NEW_PW == 1 ]] && echo '   (new — saved in .esp/secrets/admin_password)')"
[[ -n "$BACKUP_SSID" ]] && echo "  Backup:   $BACKUP_SSID"
echo

if [[ "$MONITOR" == 1 ]]; then
  info "serial log (Ctrl-C to stop):"
  exec cat "$PORT"
fi
