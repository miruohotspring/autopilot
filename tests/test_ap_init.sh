#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap init`.
# It validates create/backup/error behaviors in isolated temporary HOME dirs.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AP_BIN="$ROOT_DIR/ap"

if [[ ! -x "$AP_BIN" ]]; then
  echo "ap binary not found: $AP_BIN" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

# Assert helper: file or directory must exist.
assert_exists() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    echo "assert failed: expected to exist: $path" >&2
    exit 1
  fi
}

# Assert helper: file or directory must not exist.
assert_not_exists() {
  local path="$1"
  if [[ -e "$path" ]]; then
    echo "assert failed: expected not to exist: $path" >&2
    exit 1
  fi
}

# Assert helper: file content must match exactly.
assert_file_content() {
  local path="$1"
  local expected="$2"
  local actual
  actual="$(cat "$path")"
  if [[ "$actual" != "$expected" ]]; then
    echo "assert failed: $path content mismatch" >&2
    echo "expected: $expected" >&2
    echo "actual:   $actual" >&2
    exit 1
  fi
}

# Case 1:
# When ~/.autopilot does not exist, `ap init` should create it.
echo "[test] creates ~/.autopilot when it does not exist"
home1="$TMP_DIR/home1"
mkdir -p "$home1"
HOME="$home1" "$AP_BIN" init >/dev/null
assert_exists "$home1/.autopilot"
assert_exists "$home1/.autopilot/projects"
assert_exists "$home1/.autopilot/CLAUDE.md"
assert_exists "$home1/.autopilot/.claude/skills/self-recognition/SKILL.md"
assert_exists "$home1/.autopilot/.claude/skills/self-recognition/agents/openai.yaml"
assert_exists "$home1/.autopilot/.claude/skills/briefing/SKILL.md"
assert_exists "$home1/.autopilot/.claude/skills/briefing/agents/openai.yaml"
assert_not_exists "$home1/.autopilot.bak"

# Case 2:
# Existing ~/.autopilot should be moved to ~/.autopilot.bak,
# and a fresh ~/.autopilot should be created.
echo "[test] renames existing ~/.autopilot to ~/.autopilot.bak and recreates ~/.autopilot"
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
echo "old-state" > "$home2/.autopilot/state.txt"
HOME="$home2" "$AP_BIN" init >/dev/null
assert_exists "$home2/.autopilot"
assert_exists "$home2/.autopilot/projects"
assert_exists "$home2/.autopilot/CLAUDE.md"
assert_exists "$home2/.autopilot/.claude/skills/self-recognition/SKILL.md"
assert_exists "$home2/.autopilot/.claude/skills/briefing/SKILL.md"
assert_exists "$home2/.autopilot.bak"
assert_exists "$home2/.autopilot.bak/state.txt"
assert_file_content "$home2/.autopilot.bak/state.txt" "old-state"
assert_not_exists "$home2/.autopilot/state.txt"

# Case 3:
# If ~/.autopilot.bak already exists, command should fail and keep data untouched.
echo "[test] fails when ~/.autopilot.bak already exists"
home3="$TMP_DIR/home3"
mkdir -p "$home3/.autopilot" "$home3/.autopilot.bak"
echo "live" > "$home3/.autopilot/live.txt"
echo "backup" > "$home3/.autopilot.bak/backup.txt"
set +e
HOME="$home3" "$AP_BIN" init >/dev/null 2>&1
status=$?
set -e
if [[ "$status" -eq 0 ]]; then
  echo "assert failed: expected ap init to fail when backup exists" >&2
  exit 1
fi
assert_exists "$home3/.autopilot/live.txt"
assert_exists "$home3/.autopilot.bak/backup.txt"

# Case 4:
# Invalid invocation should return non-zero.
echo "[test] returns non-zero for invalid arguments"
set +e
"$AP_BIN" >/dev/null 2>&1
status_no_args=$?
"$AP_BIN" unknown >/dev/null 2>&1
status_unknown=$?
set -e
if [[ "$status_no_args" -eq 0 || "$status_unknown" -eq 0 ]]; then
  echo "assert failed: invalid arguments should return non-zero" >&2
  exit 1
fi

echo "all tests passed"
