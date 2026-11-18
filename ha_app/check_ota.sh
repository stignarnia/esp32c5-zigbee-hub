#!/bin/sh
# Looks for newer firmware for the hub's Zigbee devices in an OTA index (Koenkk/zigbee-OTA
# format) and leaves it on the hub as an offer. Installing is a separate, explicit step
# (Home Assistant's update entity, or the hub's UI).
#
# A device matches an index entry when its manufacturer code and image type (from its own
# Query Next Image, or its OTA client attributes) are equal, the entry's file version is
# higher than the one it runs, and the entry's optional limits (minFileVersion,
# maxFileVersion, modelId, manufacturerName) allow it. Of the matches, the highest version
# is offered.
set -eu

HUB="$1"            # host:port of the hub's web server
INDEX_URL="$2"

log() { echo "[check_ota] $*"; }

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if ! curl -fsS -m 60 -o "$tmp/index.json" "$INDEX_URL"; then
    log "index download failed: $INDEX_URL"
    exit 1
fi
if ! curl -fsS -m 15 -o "$tmp/zb.json" "http://$HUB/api/zb"; then
    log "hub not reachable at $HUB"
    exit 1
fi

# One line per device that has something newer: ieee, file_ver, url, sha512, notes (TSV)
jq -r --slurpfile idx "$tmp/index.json" '
  .devices[]
  | select(.fw.manuf_code != null and .fw.image_type != null and .fw.image_type != 65535 and .fw.file_ver != null)
  | . as $d
  | [ $idx[0][]
      | select(.manufacturerCode == $d.fw.manuf_code and .imageType == $d.fw.image_type
               and (.fileVersion // 0) > $d.fw.file_ver
               and (.url // "") != "" and (.sha512 // "") != ""
               and (.minFileVersion == null or $d.fw.file_ver >= .minFileVersion)
               and (.maxFileVersion == null or $d.fw.file_ver <= .maxFileVersion)
               and (.modelId == null or .modelId == $d.model)
               and (.manufacturerName == null or (.manufacturerName | index($d.manuf)) != null)) ]
  | max_by(.fileVersion) // empty
  | select(($d.offer.file_ver // -1) != .fileVersion)
  | [$d.ieee, (.fileVersion | tostring), .url, .sha512,
     ((.releaseNotes // "") | gsub("[\\t\\n\\r]+"; " ") | .[0:120])]
  | @tsv
' "$tmp/zb.json" > "$tmp/offers.tsv"

jq -r '.devices[] | "\(.name // .model) \(.ieee): manufacturer \(.fw.manuf_code // "?") image \(.fw.image_type // "?") file \(.fw.file_ver // "?")"' \
    "$tmp/zb.json" | while read -r line; do log "device $line"; done

n=0
while IFS="$(printf '\t')" read -r ieee ver url sha notes; do
    body="$(jq -nc --arg ieee "$ieee" --argjson ver "$ver" --arg url "$url" --arg sha "$sha" --arg notes "$notes" \
        '{action: "offer", ieee: $ieee, file_ver: $ver, url: $url, sha512: $sha, notes: $notes}')"
    if curl -fsS -m 15 -H 'X-Hub: 1' -H 'Content-Type: application/json' -d "$body" \
        "http://$HUB/api/zb/ota" > /dev/null; then
        log "offered $ieee file $ver: $url"
        n=$((n + 1))
    else
        log "offer for $ieee failed"
    fi
done < "$tmp/offers.tsv"
log "done: $n new offer(s)"
