#!/usr/bin/env bash
# Build fragmented filesystem images from a source tree (~2x data size).
# Mounts via udisksctl; populates as root through docker (ext4/ntfs are root-owned).
# Usage: make-testbases.sh [SOURCE_DIR] [OUT_DIR]
set -euo pipefail

SRC="${1:-/home/drive/test}"
OUT="${2:-$(cd "$(dirname "$0")/.." && pwd)/tests/fixtures}"
DOCKER_IMG="${DOCKER_IMG:-debian:bookworm-slim}"

mkdir -p "$OUT"
SRC=$(readlink -f "$SRC")
OUT=$(readlink -f "$OUT")

DATA_BYTES=$(du -sb "$SRC" | awk '{print $1}')
IMG_BYTES=$(( DATA_BYTES * 2 ))
IMG_BYTES=$(( (IMG_BYTES + 16*1024*1024 - 1) / (16*1024*1024) * (16*1024*1024) ))
if (( IMG_BYTES < 128*1024*1024 )); then
  IMG_BYTES=$((128*1024*1024))
fi
IMG_MIB=$(( IMG_BYTES / 1024 / 1024 ))

echo "Source: $SRC ($DATA_BYTES bytes)"
echo "Image size: ${IMG_MIB} MiB (~2x, half spare)"
echo "Output: $OUT"

ACTIVE_DEV=""
ACTIVE_MNT=""

cleanup_mount() {
  if [[ -n "$ACTIVE_DEV" && -b "$ACTIVE_DEV" ]]; then
    udisksctl unmount -b "$ACTIVE_DEV" >/dev/null 2>&1 || true
    udisksctl loop-delete -b "$ACTIVE_DEV" >/dev/null 2>&1 || true
  fi
  ACTIVE_DEV=""
  ACTIVE_MNT=""
}
trap cleanup_mount EXIT

mount_image() {
  # Sets globals: ACTIVE_DEV, ACTIVE_MNT
  local img=$1
  ACTIVE_DEV=$(udisksctl loop-setup -f "$img" | awk '{
    for (i=1;i<=NF;i++) {
      gsub(/\.$/,"",$i)
      if ($i ~ /^\/dev\/loop[0-9]+$/) { print $i; exit }
    }
  }')
  if [[ -z "$ACTIVE_DEV" || ! -b "$ACTIVE_DEV" ]]; then
    echo "loop-setup failed for $img (got '${ACTIVE_DEV:-}')" >&2
    exit 1
  fi
  ACTIVE_MNT=$(udisksctl mount -b "$ACTIVE_DEV" | awk '{print $NF}')
  if [[ ! -d "$ACTIVE_MNT" ]]; then
    echo "mount failed for $img" >&2
    exit 1
  fi
  if ! docker run --rm -v "$ACTIVE_MNT:$ACTIVE_MNT" "$DOCKER_IMG" test -w "$ACTIVE_MNT"; then
    echo "mount not writable: $ACTIVE_MNT" >&2
    exit 1
  fi
}

# Run populate script inside docker as root with mounts bound in.
docker_populate() {
  local mnt=$1
  local label=$2
  docker run --rm -i \
    -v "$mnt:$mnt" \
    -v "$SRC:$SRC:ro" \
    -e MNT="$mnt" \
    -e SRC="$SRC" \
    -e LABEL="$label" \
    "$DOCKER_IMG" \
    bash -s <<'EOS'
set -euo pipefail
fill="$MNT/.reflash-fill"
mkdir -p "$fill"

free_k=$(df -Pk "$MNT" | awk 'NR==2{print $4}')
fill_bytes=$(( free_k * 1024 * 40 / 100 ))
chunk=65536
n=$(( fill_bytes / chunk ))
if [ "$n" -lt 200 ]; then n=200; fi
if [ "$n" -gt 3000 ]; then n=3000; fi

echo "  [$LABEL] creating $n filler chunks for fragmentation..."
i=0
while [ "$i" -lt "$n" ]; do
  f=$(printf '%s/f-%05d' "$fill" "$i")
  dd if=/dev/urandom of="$f" bs="$chunk" count=1 status=none 2>/dev/null \
    || dd if=/dev/zero of="$f" bs="$chunk" count=1 status=none
  i=$((i+1))
done
sync

echo "  [$LABEL] deleting odd fillers (punch holes)..."
i=1
while [ "$i" -lt "$n" ]; do
  rm -f "$(printf '%s/f-%05d' "$fill" "$i")"
  i=$((i+2))
done
sync

echo "  [$LABEL] copying source tree into fragmented free space..."
if command -v rsync >/dev/null 2>&1; then
  rsync -a --exclude='.reflash-fill' "$SRC"/ "$MNT"/
else
  cp -a "$SRC"/. "$MNT"/
fi
sync

echo "  [$LABEL] extra fragment ops on a few files..."
count=0
# portable find without -print0 in busybox? bookworm has GNU find
while IFS= read -r f; do
  count=$((count+1))
  rem=$((count % 7))
  if [ "$rem" -eq 0 ]; then
    tmp="$f.reflash-tmp"
    cp -a "$f" "$tmp"
    mv -f "$tmp" "$f"
  fi
  if [ "$count" -gt 80 ]; then break; fi
done < <(find "$MNT" -type f ! -path '*/.reflash-fill/*' 2>/dev/null)

echo "  [$LABEL] removing remaining fillers..."
rm -rf "$fill"
sync
EOS
}

build_one() {
  local fstype=$1
  local outimg=$2
  local mkfs_cmd=$3

  echo "==> Building $outimg ($fstype)"
  cleanup_mount
  rm -f "$outimg"
  fallocate -l "${IMG_MIB}M" "$outimg"
  # shellcheck disable=SC2086
  eval "$mkfs_cmd" "$outimg"

  mount_image "$outimg"
  echo "  mounted $ACTIVE_DEV -> $ACTIVE_MNT"

  # FAT/exFAT are often user-writable; still use docker for one code path.
  docker_populate "$ACTIVE_MNT" "$fstype"

  cleanup_mount
  echo "  wrote $outimg ($(stat -c%s "$outimg") bytes)"
}

# Pull image once
docker pull "$DOCKER_IMG" >/dev/null

build_one fat32 "$OUT/testbase.fat32" "mkfs.vfat -F 32 -n TESTFAT"
build_one exfat "$OUT/testbase.exfat" "mkfs.exfat -n TESTEXFAT"
build_one ext4 "$OUT/testbase.ext4" "mkfs.ext4 -F -q -L TESTEXT4"
build_one ntfs "$OUT/testbase.ntfs" "mkfs.ntfs -F -Q -L TESTNTFS"

echo "==> Building $OUT/testbase.linear (raw copy of ext4)"
cp -a "$OUT/testbase.ext4" "$OUT/testbase.linear"

echo "Done. Testbases in $OUT:"
ls -lh "$OUT"/testbase.*
