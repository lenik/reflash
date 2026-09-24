#!/usr/bin/env bash
set -euo pipefail
prefix="${1:?prefix}"
bindir="${2:?bindir}"
datadir="${3:?datadir}"
mandir="${4:?mandir}"
build_root="${5:?build_root}"
source_root="${6:?source_root}"

mkdir -p "$bindir" "$datadir/bash-completion/completions" "$mandir/man1"
sudo ln -sfn "$build_root/reflash" "$bindir/reflash"
sudo ln -sfn "$build_root/reflash.1" "$mandir/man1/reflash.1"
sudo ln -sfn "$source_root/reflash.bash" "$datadir/bash-completion/completions/reflash"
printf "Symlinks installed under %s\n" "$prefix"
