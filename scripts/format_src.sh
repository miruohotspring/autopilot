#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

rg --files -0 src -g '*.{c,cc,cpp,cxx,h,hpp,hxx}' \
  | xargs -0 ~/.local/share/nvim/mason/bin/clang-format -i
