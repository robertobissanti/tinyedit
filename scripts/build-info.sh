#!/bin/sh
# Source identity only: no timestamps or compiler identity.
set -eu
output=${1:-bin/build_info.h}
if [ -n "${BUILD_ID:-}" ]; then
    identity=$BUILD_ID
else
    fingerprint=$(
        for file in Makefile scripts/build-info.sh inc/*.h src/*.c; do
            printf '%s\n' "$file"
            cksum < "$file"
        done | cksum | awk '{print $1 "-" $2}'
    )
    commit=$(git rev-parse --verify HEAD 2>/dev/null || true)
    if [ -n "$commit" ]; then
        identity=g$(printf '%s' "$commit" | cut -c1-12)
        if [ -n "$(git status --porcelain --untracked-files=all -- Makefile scripts inc src)" ]; then
            identity=$identity-dirty-s$fingerprint
        fi
    else
        identity=source-s$fingerprint
    fi
fi
case "$identity" in
    *[!a-zA-Z0-9._+-]*|'') echo 'Invalid BUILD_ID: use letters, digits, . _ + -' >&2; exit 1;;
esac
if [ "${#identity}" -gt 120 ]; then echo 'BUILD_ID exceeds 120 characters' >&2; exit 1; fi
mkdir -p "$(dirname "$output")"
temporary=$(mktemp "$output.XXXXXX")
trap 'rm -f "$temporary"' EXIT HUP INT TERM
printf '#define TE_BUILD_ID "%s"\n#define TE_BUILD_VERSION TE_VERSION " Build " TE_BUILD_ID\n' "$identity" > "$temporary"
if ! cmp -s "$temporary" "$output"; then mv "$temporary" "$output"; fi
