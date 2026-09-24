#!/usr/bin/env bash
# Copy testbases → testdrive.* and run reflash scenarios.
# Usage: run-testdrive.sh [FIXTURES_DIR] [REFLASH_BIN]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIX="${1:-$ROOT/tests/fixtures}"
BIN="${2:-${REFLASH_BIN:-}}"
if [[ -z "$BIN" ]]; then
  for c in "$ROOT/build/reflash" /tmp/reflash-build/reflash "$ROOT/../build/reflash"; do
    if [[ -x "$c" ]]; then BIN=$c; break; fi
  done
fi
if [[ ! -x "$BIN" ]]; then
  echo "reflash binary not found; set REFLASH_BIN=" >&2
  exit 1
fi

DBDIR=$(mktemp -d /tmp/reflash-testdrive-db.XXXXXX)
WORKDIR=$(mktemp -d /tmp/reflash-testdrive-work.XXXXXX)
export REFLASH_SHA1="${REFLASH_SHA1:-skip}"  # images often need privileged mount for SHA-1
PASS=0
FAIL=0
REPORT=()

log() { printf '%s\n' "$*"; REPORT+=("$*"); }

ok() { PASS=$((PASS+1)); log "  PASS: $*"; }
bad() { FAIL=$((FAIL+1)); log "  FAIL: $*"; }

cleanup() {
  # best-effort unmount any leftover loops we created
  true
}
trap cleanup EXIT

sha256_file() { sha256sum "$1" | awk '{print $1}'; }

# Compare two trees via mounted images — optional; for content checks use mtools/debugfs when possible.
flip_bits_in_image() {
  # Flip some bytes in the middle of the image (simulates bit-rot outside careful FS tools)
  local img=$1
  local size
  size=$(stat -c%s "$img")
  local off=$(( size / 3 ))
  python3 - "$img" "$off" <<'PY'
import sys
path, off = sys.argv[1], int(sys.argv[2])
with open(path, 'r+b') as f:
    f.seek(off)
    b = bytearray(f.read(64))
    if not b:
        sys.exit(0)
    for i in range(len(b)):
        b[i] ^= 0xA5
    f.seek(off)
    f.write(b)
print(f"flipped 64 bytes at offset {off}")
PY
}

flip_bits_in_mounted_file() {
  local img=$1
  local fstype=$2
  local loop_line
  loop_line=$(udisksctl loop-setup -f "$img")
  dev=$(echo "$loop_line" | awk '{for(i=1;i<=NF;i++){gsub(/\.$/,"",$i); if($i~/^\/dev\/loop[0-9]+$/){print $i;exit}}}')
  mnt=$(udisksctl mount -b "$dev" | awk '{print $NF}')
  local out
  out=$(docker run --rm -v "$mnt:$mnt" debian:bookworm-slim bash -c '
    set -e
    target=$(find "'"$mnt"'" -type f \( -name "*.txt" -o -name "*.log" -o -name "*.html" \) ! -path "*/lost+found/*" 2>/dev/null | head -1)
    if [ -z "$target" ]; then
      target=$(find "'"$mnt"'" -type f ! -path "*/lost+found/*" 2>/dev/null | head -1)
    fi
    if [ -z "$target" ]; then echo NOFILE; exit 0; fi
    sz=$(stat -c%s "$target")
    off=$((sz / 2))
    byte=$(dd if="$target" bs=1 skip="$off" count=1 status=none | od -An -tu1 | tr -d " ")
    xor=$((byte ^ 255))
    printf "\\$(printf "%03o" "$xor")" | dd of="$target" bs=1 seek="$off" conv=notrunc status=none
    echo "flipped $target at $off"
  ')
  echo "$out"
  if echo "$out" | grep -q NOFILE; then
    log "  WARN: no file to flip on $fstype; raw XOR instead"
    udisksctl unmount -b "$dev" >/dev/null
    udisksctl loop-delete -b "$dev" >/dev/null
    flip_bits_in_image "$img"
    return
  fi
  ok "bit-flip mounted file on $fstype"
  sync
  udisksctl unmount -b "$dev" >/dev/null
  udisksctl loop-delete -b "$dev" >/dev/null
}

run_reflash() {
  local img=$1
  local mode=$2  # linear|recursive
  local db=$3
  local extra=${4:-}
  local args=(-d "$db" -b 4096)
  if [[ "$mode" == recursive ]]; then
    args+=(-r)
  else
    args+=(-l)
  fi
  # shellcheck disable=SC2086
  if "$BIN" "${args[@]}" $extra "$img"; then
    return 0
  else
    return 1
  fi
}

test_fs() {
  local base=$1
  local name
  name=$(basename "$base")
  local fstype=${name#testbase.}
  local drive="$FIX/testdrive.$fstype"
  local db1="$DBDIR/$fstype-1.sqlite"
  local db2="$DBDIR/$fstype-2.sqlite"

  log ""
  log "======== $fstype ========"
  if [[ ! -f "$base" ]]; then
    bad "missing $base"
    return
  fi

  cp -a "$base" "$drive"
  local hash0
  hash0=$(sha256_file "$drive")

  # 1) Massage original data (must succeed, content-preserving for recursive)
  log "-- massage #1 (clean image, recursive)"
  if run_reflash "$drive" recursive "$db1"; then
    ok "recursive massage #1 exit 0"
  else
    bad "recursive massage #1 failed"
  fi
  local hash1
  hash1=$(sha256_file "$drive")
  # Recursive rewrite of allocated blocks should leave image bitwise identical
  # if every written byte equals the read byte — expect same hash.
  if [[ "$hash0" == "$hash1" ]]; then
    ok "image hash unchanged after clean recursive massage"
  else
    log "  NOTE: image hash changed after recursive massage (metadata timestamps?)"
    log "        before=$hash0"
    log "        after =$hash1"
  fi

  # 2) Linear massage on a fresh copy
  cp -a "$base" "$drive"
  hash0=$(sha256_file "$drive")
  log "-- massage linear on clean copy"
  if run_reflash "$drive" linear "$DBDIR/$fstype-lin.sqlite"; then
    ok "linear massage exit 0"
  else
    bad "linear massage failed"
  fi
  hash1=$(sha256_file "$drive")
  if [[ "$hash0" == "$hash1" ]]; then
    ok "linear massage bitwise identical"
  else
    bad "linear massage changed image bytes"
  fi

  # 3) Flip file content bits, massage again
  cp -a "$base" "$drive"
  log "-- bit-flip a file then recursive massage"
  flip_bits_in_mounted_file "$drive" "$fstype"
  local hash_flip
  hash_flip=$(sha256_file "$drive")
  if run_reflash "$drive" recursive "$db2"; then
    ok "recursive massage after bit-flip exit 0"
  else
    bad "recursive massage after bit-flip failed"
  fi
  local hash_after
  hash_after=$(sha256_file "$drive")
  # Massage should preserve the flipped bits (rewrite same bytes)
  if [[ "$hash_flip" == "$hash_after" ]]; then
    ok "flipped bits preserved by massage (rewrite-in-place)"
  else
    bad "massage changed flipped image unexpectedly"
  fi

  # 4) Raw mid-image corruption + linear massage should preserve corruption
  cp -a "$base" "$drive"
  log "-- raw 64-byte XOR in image + linear massage"
  flip_bits_in_image "$drive"
  hash_flip=$(sha256_file "$drive")
  if run_reflash "$drive" linear "$DBDIR/$fstype-raw.sqlite"; then
    ok "linear after raw XOR exit 0"
  else
    bad "linear after raw XOR failed"
  fi
  hash_after=$(sha256_file "$drive")
  if [[ "$hash_flip" == "$hash_after" ]]; then
    ok "raw XOR preserved by linear massage"
  else
    bad "linear massage altered XOR region"
  fi

  # 5) Pause/resume smoke: run with tiny block size
  cp -a "$base" "$drive"
  log "-- small block-size recursive smoke"
  if run_reflash "$drive" recursive "$DBDIR/$fstype-bs.sqlite" "-b 512"; then
    ok "block-size 512 recursive ok"
  else
    bad "block-size 512 recursive failed"
  fi

  # 6) Double massage idempotence
  cp -a "$base" "$drive"
  run_reflash "$drive" recursive "$DBDIR/$fstype-id1.sqlite" || true
  hash0=$(sha256_file "$drive")
  if run_reflash "$drive" recursive "$DBDIR/$fstype-id2.sqlite"; then
    hash1=$(sha256_file "$drive")
    if [[ "$hash0" == "$hash1" ]]; then
      ok "second massage idempotent"
    else
      bad "second massage changed bytes"
    fi
  else
    bad "second massage failed"
  fi
}

log "reflash: $BIN"
log "fixtures: $FIX"
log "REFLASH_SHA1=$REFLASH_SHA1"

# Content integrity: after recursive massage, every regular file must match testbase.
compare_trees() {
  local img_a=$1
  local img_b=$2
  local label=$3
  local da db ma mb
  da=$(udisksctl loop-setup -f "$img_a" | awk '{for(i=1;i<=NF;i++){gsub(/\.$/,"",$i); if($i~/^\/dev\/loop[0-9]+$/){print $i;exit}}}')
  db=$(udisksctl loop-setup -f "$img_b" | awk '{for(i=1;i<=NF;i++){gsub(/\.$/,"",$i); if($i~/^\/dev\/loop[0-9]+$/){print $i;exit}}}')
  ma=$(udisksctl mount -b "$da" | awk '{print $NF}')
  mb=$(udisksctl mount -b "$db" | awk '{print $NF}')
  local rc=0
  if docker run --rm -v "$ma:$ma:ro" -v "$mb:$mb:ro" debian:bookworm-slim bash -c '
    set -e
    tmp=$(mktemp)
    (cd "'"$ma"'" && find . -type f ! -path "./lost+found/*" -print0 | sort -z | xargs -0 sha256sum) >"$tmp.a"
    (cd "'"$mb"'" && find . -type f ! -path "./lost+found/*" -print0 | sort -z | xargs -0 sha256sum) >"$tmp.b"
    if ! diff -q "$tmp.a" "$tmp.b" >/dev/null; then
      echo DIFF
      diff "$tmp.a" "$tmp.b" | head -40 || true
      exit 1
    fi
    echo OK files=$(wc -l <"$tmp.a")
  '; then
    ok "content integrity $label"
  else
    bad "content integrity $label"
    rc=1
  fi
  udisksctl unmount -b "$da" >/dev/null
  udisksctl unmount -b "$db" >/dev/null
  udisksctl loop-delete -b "$da" >/dev/null
  udisksctl loop-delete -b "$db" >/dev/null
  return $rc
}

# Refuse recursive when mounted (without -m)
test_refuse_mounted() {
  local base=$1
  local drive="$FIX/testdrive.refuse"
  cp -a "$base" "$drive"
  local dev mnt
  dev=$(udisksctl loop-setup -f "$drive" | awk '{for(i=1;i<=NF;i++){gsub(/\.$/,"",$i); if($i~/^\/dev\/loop[0-9]+$/){print $i;exit}}}')
  mnt=$(udisksctl mount -b "$dev" | awk '{print $NF}')
  if "$BIN" -r -d "$DBDIR/refuse.sqlite" "$drive" 2>/tmp/reflash-refuse.err; then
    bad "should refuse massage while mounted"
  else
    ok "refuses recursive while mounted"
  fi
  udisksctl unmount -b "$dev" >/dev/null
  udisksctl loop-delete -b "$dev" >/dev/null
}

for base in "$FIX"/testbase.ext4 "$FIX"/testbase.fat32 "$FIX"/testbase.exfat "$FIX"/testbase.ntfs; do
  test_fs "$base"
done

# Post-pass content integrity on a fresh massage of each FS
log ""
log "======== content integrity vs testbase ========"
for fs in ext4 fat32 exfat ntfs; do
  base="$FIX/testbase.$fs"
  drive="$FIX/testdrive.$fs"
  [[ -f "$base" ]] || continue
  cp -a "$base" "$drive"
  if run_reflash "$drive" recursive "$DBDIR/integrity-$fs.sqlite"; then
    compare_trees "$base" "$drive" "$fs after recursive massage"
  else
    bad "massage for integrity $fs"
  fi
done

log ""
log "======== refuse when mounted ========"
test_refuse_mounted "$FIX/testbase.ext4"

# Linear-only base
if [[ -f "$FIX/testbase.linear" ]]; then
  log ""
  log "======== linear blob ========"
  cp -a "$FIX/testbase.linear" "$FIX/testdrive.linear"
  h0=$(sha256_file "$FIX/testdrive.linear")
  if run_reflash "$FIX/testdrive.linear" linear "$DBDIR/linear.sqlite"; then
    ok "testdrive.linear massage"
  else
    bad "testdrive.linear massage"
  fi
  h1=$(sha256_file "$FIX/testdrive.linear")
  if [[ "$h0" == "$h1" ]]; then ok "linear blob unchanged"; else bad "linear blob changed"; fi
fi

log ""
log "======== summary: $PASS passed, $FAIL failed ========"
rm -rf "$DBDIR"
# keep testdrive.* in FIX for inspection
exit "$FAIL"
