#!/usr/bin/env bash
set -euo pipefail
bindir="${1:?bindir}"
datadir="${2:?datadir}"
mandir="${3:?mandir}"

for p in \
    "$bindir/sdmsg" \
    "$mandir/man1/sdmsg.1" \
    "$datadir/bash-completion/completions/sdmsg"
do
    if [ -L "$p" ]; then
        sudo rm -f "$p"
        printf "Removed %s\n" "$p"
    else
        printf "Skipped (not a symlink): %s\n" "$p"
    fi
done
