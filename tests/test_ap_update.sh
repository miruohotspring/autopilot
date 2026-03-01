#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap update`.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AP_BIN="$ROOT_DIR/ap"

if [[ ! -x "$AP_BIN" ]]; then
  echo "ap binary not found: $AP_BIN" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

assert_exists() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    echo "assert failed: expected to exist: $path" >&2
    exit 1
  fi
}

assert_file_contains() {
  local path="$1"
  local expected="$2"
  if ! grep -q --fixed-strings -- "$expected" "$path"; then
    echo "assert failed: expected '$expected' in $path" >&2
    echo "--- file content ---" >&2
    cat "$path" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

assert_same_file() {
  local actual="$1"
  local expected="$2"
  if ! cmp -s "$actual" "$expected"; then
    echo "assert failed: files differ" >&2
    echo "actual  : $actual" >&2
    echo "expected: $expected" >&2
    exit 1
  fi
}

# Case 1:
# Without ~/.autopilot, command should fail with guidance.
echo "[test] fails when ~/.autopilot does not exist"
home1="$TMP_DIR/home1"
mkdir -p "$home1"
stderr1="$TMP_DIR/stderr1.txt"
set +e
(cd "$ROOT_DIR" && HOME="$home1" "$AP_BIN" update 2>"$stderr1")
status1=$?
set -e
if [[ "$status1" -eq 0 ]]; then
  echo "assert failed: expected ap update to fail when ~/.autopilot is missing" >&2
  exit 1
fi
assert_file_contains "$stderr1" "Please run ap init first"

# Case 2:
# Running from repo root should sync managed templates into ~/.autopilot.
echo "[test] syncs managed files from templates/autopilot"
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
echo "old content" >"$home2/.autopilot/CLAUDE.md"
stdout2="$TMP_DIR/stdout2.txt"
stderr2="$TMP_DIR/stderr2.txt"
set +e
(cd "$ROOT_DIR" && HOME="$home2" "$AP_BIN" update >"$stdout2" 2>"$stderr2")
status2=$?
set -e
if [[ "$status2" -ne 0 ]]; then
  echo "assert failed: expected ap update to succeed from repository root" >&2
  cat "$stderr2" >&2
  exit 1
fi

for rel in \
  "CLAUDE.md" \
  ".claude/skills/self-recognition/SKILL.md" \
  ".claude/skills/self-recognition/agents/openai.yaml" \
  ".claude/skills/briefing/SKILL.md" \
  ".claude/skills/briefing/agents/openai.yaml"; do
  assert_exists "$home2/.autopilot/$rel"
  assert_same_file "$home2/.autopilot/$rel" "$ROOT_DIR/templates/autopilot/$rel"
done
assert_file_contains "$stdout2" "updated: \"$home2/.autopilot/CLAUDE.md\""

# Case 3:
# Outside repo root, command should fail to locate template directory.
echo "[test] fails outside repository root"
home3="$TMP_DIR/home3"
mkdir -p "$home3/.autopilot"
stderr3="$TMP_DIR/stderr3.txt"
set +e
(cd "$TMP_DIR" && HOME="$home3" "$AP_BIN" update 2>"$stderr3")
status3=$?
set -e
if [[ "$status3" -eq 0 ]]; then
  echo "assert failed: expected ap update to fail outside repository root" >&2
  exit 1
fi
assert_file_contains "$stderr3" "run from autopilot repository root"

echo "all tests passed"
