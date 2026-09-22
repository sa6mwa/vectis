#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

bash "$script_dir/verify_release_artifacts.sh"
bash "$script_dir/verify_release_privacy.sh"

echo "package verification ok"
