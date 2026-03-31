#!/usr/bin/env bash
set -eu

label=${1:?label required}
shift

start=$(date +%s)
"$@"
end=$(date +%s)

printf '[%s] %ss\n' "$label" "$((end - start))"

