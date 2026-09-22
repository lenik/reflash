#!/usr/bin/env bash
set -euo pipefail
prefix="${1:?prefix}"
bindir="${2:?bindir}"
datadir="${3:?datadir}"
mandir="${4:?mandir}"
build_root="${5:?build_root}"
source_root="${6:?source_root}"

mkdir -p "$bindir" "$datadir/bash-completion/completions" "$mandir/man1"
sudo ln -sfn "$build_root/sdmsg" "$bindir/sdmsg"
sudo ln -sfn "$build_root/sdmsg.1" "$mandir/man1/sdmsg.1"
sudo ln -sfn "$source_root/sdmsg.bash" "$datadir/bash-completion/completions/sdmsg"
printf "Symlinks installed under %s\n" "$prefix"
