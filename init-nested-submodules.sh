#!/bin/bash
set -euo pipefail

# Some hosts/CI systems clone this repo's own submodules (e.g.
# backend/external/<lib>) automatically, but don't recurse into
# THAT submodule's own .gitmodules file if it has nested
# submodules of its own.
#
# This script finds every .gitmodules file in the tree -- at any
# depth -- and clones any submodule path that's still empty. It
# works without .git metadata, since .gitmodules is a plain
# tracked text file that survives a plain `COPY . .` even when
# the actual submodule contents don't.
#
# A nested submodule's own .gitmodules only exists AFTER its parent
# has been cloned, so one pass is not enough: each pass can reveal
# new .gitmodules files. The script therefore repeats until a pass
# clones nothing (bounded, in case of a cycle).
#
# NOTE: it clones each library's default branch (--depth 1), not the
# commit this repo pins, because a plain `COPY . .` has no .git to
# say which commit that is. For pinned builds, check out with
# `git submodule update --init --recursive` first: populated
# directories are left untouched here.

MAX_PASSES=10

for pass in $(seq 1 "$MAX_PASSES"); do
    cloned=0

    while read -r modfile; do
        dir=$(dirname "$modfile")
        echo "[pass $pass] Found .gitmodules in: $dir"

        while read -r key path; do
            name=$(echo "$key" | sed -E 's/^submodule\.(.*)\.path$/\1/')
            url=$(git config -f "$modfile" --get "submodule.$name.url")
            target="$dir/$path"

            if [ -z "$(ls -A "$target" 2>/dev/null || true)" ]; then
                echo "  Cloning missing submodule: $target <- $url"
                rm -rf "$target"
                git clone --depth 1 "$url" "$target"
                cloned=1
            else
                echo "  Already present: $target"
            fi
        done < <(git config -f "$modfile" --get-regexp '\.path$')
    done < <(find . -name ".gitmodules")

    if [ "$cloned" -eq 0 ]; then
        echo "All submodules present (after $pass pass(es))."
        exit 0
    fi
done

echo "ERROR: submodules were still being discovered after $MAX_PASSES passes; giving up." >&2
exit 1
