#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap list`.
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
AP_BIN="$ROOT_DIR/ap"

if [[ ! -x "$AP_BIN" ]]; then
  echo "ap binary not found: $AP_BIN" >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

assert_file_contains() {
  local path="$1"
  local expected="$2"
  if ! grep -q --fixed-strings "$expected" "$path"; then
    echo "assert failed: expected '$expected' in $path" >&2
    echo "--- file content ---" >&2
    cat "$path" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

assert_line_equals() {
  local path="$1"
  local lineno="$2"
  local expected="$3"
  local actual
  actual="$(sed -n "${lineno}p" "$path")"
  if [[ "$actual" != "$expected" ]]; then
    echo "assert failed: expected line ${lineno} in $path to be '$expected'" >&2
    echo "--- file content ---" >&2
    cat "$path" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

# Case 1:
# Without ~/.autopilot, command should fail with guidance.
echo "[test] fails when ~/.autopilot does not exist"
home1="$TMP_DIR/home1"
mkdir -p "$home1"
stderr1="$TMP_DIR/list_stderr1.txt"
set +e
HOME="$home1" "$AP_BIN" list 2>"$stderr1" >/dev/null
status1=$?
set -e
if [[ "$status1" -eq 0 ]]; then
  echo "assert failed: expected ap list to fail when ~/.autopilot is missing" >&2
  exit 1
fi
assert_file_contains "$stderr1" "Please run ap init first"

# Case 2:
# Missing projects file should print no projects.
echo "[test] prints no projects when projects.yaml is missing"
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
stdout2="$TMP_DIR/list_stdout2.txt"
HOME="$home2" "$AP_BIN" list >"$stdout2"
assert_file_contains "$stdout2" "no projects"

# Case 3:
# Existing projects should be listed one per line in sorted order.
echo "[test] lists existing projects"
HOME="$home2" "$AP_BIN" new Zebra >/dev/null
HOME="$home2" "$AP_BIN" new Alpha >/dev/null
stdout3="$TMP_DIR/list_stdout3.txt"
HOME="$home2" "$AP_BIN" list >"$stdout3"
assert_line_equals "$stdout3" 1 "Alpha"
assert_line_equals "$stdout3" 2 "Zebra"

# Case 4:
# Invalid invocation should fail.
echo "[test] rejects unexpected arguments"
set +e
HOME="$home2" "$AP_BIN" list extra >/dev/null 2>&1
status4=$?
set -e
if [[ "$status4" -eq 0 ]]; then
  echo "assert failed: expected ap list with extra args to fail" >&2
  exit 1
fi

echo "all tests passed"
