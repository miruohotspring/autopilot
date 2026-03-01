#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap new`.
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
  if ! grep -q --fixed-strings "$expected" "$path"; then
    echo "assert failed: expected '$expected' in $path" >&2
    echo "--- file content ---" >&2
    cat "$path" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

assert_file_equals() {
  local path="$1"
  local expected="$2"
  local actual
  actual="$(cat "$path")"
  if [[ "$actual" != "$expected" ]]; then
    echo "assert failed: expected exact content in $path" >&2
    echo "expected: $expected" >&2
    echo "actual  : $actual" >&2
    exit 1
  fi
}

assert_project_block() {
  local path="$1"
  local project="$2"
  if ! awk -v project="$project" '
    $0 == project ":" {
      if (getline line1 <= 0) next
      if (getline line2 <= 0) next
      if (line1 == "  priority: 1" && line2 == "  paths: []") {
        found = 1
      }
    }
    END { exit(found ? 0 : 1) }
  ' "$path"; then
    echo "assert failed: expected project block for $project in $path" >&2
    echo "--- file content ---" >&2
    cat "$path" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

run_expect_fail() {
  set +e
  "$@"
  local status=$?
  set -e
  if [[ "$status" -eq 0 ]]; then
    echo "assert failed: expected command to fail: $*" >&2
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
HOME="$home1" "$AP_BIN" new ProjectA 2>"$stderr1"
status1=$?
set -e
if [[ "$status1" -eq 0 ]]; then
  echo "assert failed: expected ap new to fail when ~/.autopilot is missing" >&2
  exit 1
fi
assert_file_contains "$stderr1" "Please run ap init first"

# Case 2:
# Creates projects.yaml when missing, appends project, and creates project directory.
echo "[test] creates ~/.autopilot/projects.yaml, adds project, and creates project directory"
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
HOME="$home2" "$AP_BIN" new Project-1 >/dev/null
assert_exists "$home2/.autopilot/projects.yaml"
assert_exists "$home2/.autopilot/projects/Project-1"
assert_exists "$home2/.autopilot/projects/Project-1/TODO.md"
assert_exists "$home2/.autopilot/projects/Project-1/dashboard.md"
assert_project_block "$home2/.autopilot/projects.yaml" "Project-1"
assert_file_equals "$home2/.autopilot/projects/Project-1/TODO.md" "# Project-1 TODO"
assert_file_equals "$home2/.autopilot/projects/Project-1/dashboard.md" "# Project-1 Dashboard"

# Case 3:
# Duplicate project should fail.
echo "[test] fails for duplicate project names"
stderr3="$TMP_DIR/stderr3.txt"
set +e
HOME="$home2" "$AP_BIN" new Project-1 2>"$stderr3"
status3=$?
set -e
if [[ "$status3" -eq 0 ]]; then
  echo "assert failed: expected duplicate project name to fail" >&2
  exit 1
fi
assert_file_contains "$stderr3" "project already exists"

# Case 4:
# Invalid names should fail.
echo "[test] validates project name format"
run_expect_fail env HOME="$home2" "$AP_BIN" new "-bad"
run_expect_fail env HOME="$home2" "$AP_BIN" new "bad-"
run_expect_fail env HOME="$home2" "$AP_BIN" new "bad_name"

# Case 5:
# Interactive mode should prompt and create project.
echo "[test] supports interactive project name input"
stdout5="$TMP_DIR/stdout5.txt"
printf 'Interactive9\n' | HOME="$home2" "$AP_BIN" new >"$stdout5"
assert_file_contains "$stdout5" "Enter your new project name:"
assert_project_block "$home2/.autopilot/projects.yaml" "Interactive9"
assert_exists "$home2/.autopilot/projects/Interactive9"
assert_file_equals "$home2/.autopilot/projects/Interactive9/TODO.md" "# Interactive9 TODO"
assert_file_equals "$home2/.autopilot/projects/Interactive9/dashboard.md" "# Interactive9 Dashboard"

echo "all tests passed"
