#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-or-later
# Meson run_target helper: bash scripts/posync.sh <SOURCE_ROOT> [VERSION]
set -euo pipefail
SOURCE_ROOT="${1:-${MESON_SOURCE_ROOT:-.}}"
VERSION="${2:-}"
cd "$SOURCE_ROOT/po"
if [ -z "$VERSION" ] && [ -f "$SOURCE_ROOT/VERSION" ]; then
    VERSION=$(head -n1 "$SOURCE_ROOT/VERSION" | tr -d '\r')
    VERSION=${VERSION#v}
fi
VERSION=${VERSION:-0.0.0}

xgettext --from-code=UTF-8 --keyword=_ --keyword=N_ --language=C++ \
    --directory=.. --output=reflash.pot --files-from=POTFILES \
    --package-name=reflash --package-version="$VERSION" \
    --msgid-bugs-address=reflash@bodz.net --no-wrap
sed -i 's/charset=CHARSET/charset=UTF-8/' reflash.pot

while IFS= read -r lang; do
    [ -n "$lang" ] || continue
    case "$lang" in \#*) continue ;; esac
    po_file="$lang.po"
    if [ ! -f "$po_file" ]; then
        msginit --no-translator --input=reflash.pot --locale="$lang" \
            --output-file="$po_file" --no-wrap
    fi
    msgmerge --update --backup=none --no-wrap "$po_file" reflash.pot
    tmp=$(mktemp "$lang.XXXXXX.po")
    msgattrib --no-obsolete --no-wrap --output-file="$tmp" "$po_file"
    mv -f "$tmp" "$po_file"
done < LINGUAS
