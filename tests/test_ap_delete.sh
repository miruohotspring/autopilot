#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap delete`.
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

assert_project_exists() {
  local path="$1"
  local project="$2"
  if ! awk -v project="$project" '
    $0 == project ":" { found = 1 }
    END { exit(found ? 0 : 1) }
  ' "$path"; then
    echo "assert failed: expected project $project in $path" >&2
    echo "--- file content ---" >&2
    cat "$path" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

assert_project_not_exists() {
  local path="$1"
  local project="$2"
  if awk -v project="$project" '
    $0 == project ":" { found = 1 }
    END { exit(found ? 0 : 1) }
  ' "$path"; then
    echo "assert failed: expected project $project to be removed from $path" >&2
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
stderr1="$TMP_DIR/delete_stderr1.txt"
set +e
printf 'y\n' | HOME="$home1" "$AP_BIN" delete ProjectA 2>"$stderr1" >/dev/null
status1=$?
set -e
if [[ "$status1" -eq 0 ]]; then
  echo "assert failed: expected ap delete to fail when ~/.autopilot is missing" >&2
  exit 1
fi
assert_file_contains "$stderr1" "Please run ap init first"

# Case 2:
# Missing projects file should fail.
echo "[test] fails when projects.yaml does not exist"
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
stderr2="$TMP_DIR/delete_stderr2.txt"
set +e
printf 'y\n' | HOME="$home2" "$AP_BIN" delete ProjectA 2>"$stderr2" >/dev/null
status2=$?
set -e
if [[ "$status2" -eq 0 ]]; then
  echo "assert failed: expected ap delete to fail when projects.yaml is missing" >&2
  exit 1
fi
assert_file_contains "$stderr2" "project not found"

# Prepare projects for delete scenarios.
HOME="$home2" "$AP_BIN" new KeepMe >/dev/null
HOME="$home2" "$AP_BIN" new DeleteMe >/dev/null
projects_file="$home2/.autopilot/projects.yaml"
assert_project_exists "$projects_file" "KeepMe"
assert_project_exists "$projects_file" "DeleteMe"

# Case 3:
# Deleting with confirmation y should remove the project.
echo "[test] deletes project when confirmed"
stdout3="$TMP_DIR/delete_stdout3.txt"
printf 'y\n' | HOME="$home2" "$AP_BIN" delete DeleteMe >"$stdout3"
assert_file_contains "$stdout3" "Delete project 'DeleteMe'? [y/n]:"
assert_file_contains "$stdout3" "deleted project: DeleteMe"
assert_project_not_exists "$projects_file" "DeleteMe"
assert_project_exists "$projects_file" "KeepMe"

# Case 4:
# n confirmation should keep project untouched.
echo "[test] cancels delete when user answers n"
stdout4="$TMP_DIR/delete_stdout4.txt"
set +e
printf 'n\n' | HOME="$home2" "$AP_BIN" delete KeepMe >"$stdout4" 2>/dev/null
status4=$?
set -e
if [[ "$status4" -eq 0 ]]; then
  echo "assert failed: expected ap delete to return non-zero when canceled" >&2
  exit 1
fi
assert_file_contains "$stdout4" "Delete project 'KeepMe'? [y/n]:"
assert_file_contains "$stdout4" "canceled"
assert_project_exists "$projects_file" "KeepMe"

# Case 5:
# Interactive mode should show projects and allow selecting by number.
echo "[test] supports interactive project selection"
home3="$TMP_DIR/home3"
mkdir -p "$home3/.autopilot"
HOME="$home3" "$AP_BIN" new InteractiveDel >/dev/null
projects_file3="$home3/.autopilot/projects.yaml"
stdout5="$TMP_DIR/delete_stdout5.txt"
printf '1\ny\n' | HOME="$home3" "$AP_BIN" delete >"$stdout5"
assert_file_contains "$stdout5" "Select project to delete:"
assert_file_contains "$stdout5" "1) InteractiveDel"
assert_file_contains "$stdout5" "Enter number to delete:"
assert_file_contains "$stdout5" "Delete project 'InteractiveDel'? [y/n]:"
assert_project_not_exists "$projects_file3" "InteractiveDel"

# Case 6:
# Unknown and invalid names should fail.
echo "[test] validates target project name and existence"
run_expect_fail env HOME="$home2" "$AP_BIN" delete "-bad"
stderr6="$TMP_DIR/delete_stderr6.txt"
set +e
printf 'y\n' | HOME="$home2" "$AP_BIN" delete NotExists 2>"$stderr6" >/dev/null
status6=$?
set -e
if [[ "$status6" -eq 0 ]]; then
  echo "assert failed: expected unknown project delete to fail" >&2
  exit 1
fi
assert_file_contains "$stderr6" "project not found"

echo "all tests passed"
