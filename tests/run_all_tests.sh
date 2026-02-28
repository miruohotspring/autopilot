#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

bash "$ROOT_DIR/tests/test_ap_init.sh"
bash "$ROOT_DIR/tests/test_ap_new.sh"
bash "$ROOT_DIR/tests/test_ap_delete.sh"
bash "$ROOT_DIR/tests/test_ap_list.sh"
