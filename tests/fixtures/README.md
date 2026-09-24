# Regenerated FS images for reflash integration tests
#
# Build (from repo root):
#   ./scripts/make-testbases.sh /home/drive/test tests/fixtures
#
# Run scenarios:
#   REFLASH_BIN=/tmp/reflash-build/reflash ./scripts/run-testdrive.sh
#
# Images are ~2x the source tree size (half data / half spare), intentionally
# fragmented. They are gitignored — rebuild locally when needed.
#
# Expected files:
#   testbase.ext4  testbase.fat32  testbase.exfat  testbase.ntfs  testbase.linear
#   testdrive.*    (working copies produced by run-testdrive.sh)
