#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap rm`.
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

assert_file_not_contains() {
  local path="$1"
  local expected="$2"
  if grep -q --fixed-strings "$expected" "$path"; then
    echo "assert failed: expected '$expected' not to appear in $path" >&2
    echo "--- file content ---" >&2
    cat "$path" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

assert_project_has_path() {
  local file="$1"
  local project="$2"
  local path_value="$3"
  if ! awk -v project="$project" -v path_value="$path_value" '
    $0 == project ":" { in_target = 1; next }
    in_target && $0 ~ /^[^ \t#][^:]*:/ { in_target = 0 }
    in_target && $0 == "    - \047" path_value "\047" { found = 1 }
    END { exit(found ? 0 : 1) }
  ' "$file"; then
    echo "assert failed: expected path '$path_value' in project '$project'" >&2
    echo "--- file content ---" >&2
    cat "$file" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

assert_project_not_has_path() {
  local file="$1"
  local project="$2"
  local path_value="$3"
  if awk -v project="$project" -v path_value="$path_value" '
    $0 == project ":" { in_target = 1; next }
    in_target && $0 ~ /^[^ \t#][^:]*:/ { in_target = 0 }
    in_target && $0 == "    - \047" path_value "\047" { found = 1 }
    END { exit(found ? 0 : 1) }
  ' "$file"; then
    echo "assert failed: expected path '$path_value' to be removed from project '$project'" >&2
    echo "--- file content ---" >&2
    cat "$file" >&2
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
stderr1="$TMP_DIR/rm_stderr1.txt"
set +e
HOME="$home1" "$AP_BIN" rm -p AlphaProject 2>"$stderr1" >/dev/null
status1=$?
set -e
if [[ "$status1" -eq 0 ]]; then
  echo "assert failed: expected ap rm to fail when ~/.autopilot is missing" >&2
  exit 1
fi
assert_file_contains "$stderr1" "Please run ap init first"

# Case 2:
# Missing projects file should fail.
echo "[test] fails when projects.yaml does not exist"
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
stderr2="$TMP_DIR/rm_stderr2.txt"
set +e
HOME="$home2" "$AP_BIN" rm -p AlphaProject 2>"$stderr2" >/dev/null
status2=$?
set -e
if [[ "$status2" -eq 0 ]]; then
  echo "assert failed: expected ap rm to fail when projects.yaml is missing" >&2
  exit 1
fi
assert_file_contains "$stderr2" "project not found"

# Prepare projects and paths for rm scenarios.
HOME="$home2" "$AP_BIN" new AlphaProject >/dev/null
HOME="$home2" "$AP_BIN" new BetaProject >/dev/null
path_a1="$TMP_DIR/p-a1"
path_a2="$TMP_DIR/p-a2"
path_b1="$TMP_DIR/p-b1"
mkdir -p "$path_a1" "$path_a2" "$path_b1"
HOME="$home2" "$AP_BIN" add "$path_a1" -p AlphaProject >/dev/null
HOME="$home2" "$AP_BIN" add "$path_a2" -p AlphaProject >/dev/null
HOME="$home2" "$AP_BIN" add "$path_b1" -p BetaProject >/dev/null
projects_file="$home2/.autopilot/projects.yaml"

# Case 3:
# With -p, user can select path and remove it when confirmed.
echo "[test] removes selected path when confirmed"
stdout3="$TMP_DIR/rm_stdout3.txt"
printf '1\ny\n' | HOME="$home2" "$AP_BIN" rm -p AlphaProject >"$stdout3"
assert_file_contains "$stdout3" "Select path to remove:"
assert_file_contains "$stdout3" "Enter number to remove:"
assert_file_contains "$stdout3" "Remove path '"
assert_file_contains "$stdout3" "removed path:"
assert_project_not_has_path "$projects_file" "AlphaProject" "$path_a1"
assert_project_has_path "$projects_file" "AlphaProject" "$path_a2"

# Case 4:
# n confirmation should cancel removal.
echo "[test] cancels rm when user answers n"
stdout4="$TMP_DIR/rm_stdout4.txt"
set +e
printf '1\nn\n' | HOME="$home2" "$AP_BIN" rm -p AlphaProject >"$stdout4" 2>/dev/null
status4=$?
set -e
if [[ "$status4" -eq 0 ]]; then
  echo "assert failed: expected ap rm to return non-zero when canceled" >&2
  exit 1
fi
assert_file_contains "$stdout4" "canceled"
assert_project_has_path "$projects_file" "AlphaProject" "$path_a2"

# Case 5:
# Without -p, command should ask project then path, then remove after y.
echo "[test] supports interactive project and path selection"
stdout5="$TMP_DIR/rm_stdout5.txt"
printf '2\n1\ny\n' | HOME="$home2" "$AP_BIN" rm >"$stdout5"
assert_file_contains "$stdout5" "Select project to remove path:"
assert_file_contains "$stdout5" "Enter number to remove path:"
assert_file_contains "$stdout5" "Select path to remove:"
assert_file_contains "$stdout5" "Enter number to remove:"
assert_project_not_has_path "$projects_file" "BetaProject" "$path_b1"
assert_file_not_contains "$stdout5" "[y/n]:\nDelete project"

# Case 6:
# Project with no paths should fail.
echo "[test] fails when selected project has no paths"
HOME="$home2" "$AP_BIN" new EmptyProject >/dev/null
stderr6="$TMP_DIR/rm_stderr6.txt"
set +e
HOME="$home2" "$AP_BIN" rm -p EmptyProject 2>"$stderr6" >/dev/null
status6=$?
set -e
if [[ "$status6" -eq 0 ]]; then
  echo "assert failed: expected ap rm to fail when project has no paths" >&2
  exit 1
fi
assert_file_contains "$stderr6" "path not found"

# Case 7:
# Unknown and invalid project names should fail.
echo "[test] validates target project name and existence"
run_expect_fail env HOME="$home2" "$AP_BIN" rm -p "-bad"
stderr7="$TMP_DIR/rm_stderr7.txt"
set +e
HOME="$home2" "$AP_BIN" rm -p NotExists 2>"$stderr7" >/dev/null
status7=$?
set -e
if [[ "$status7" -eq 0 ]]; then
  echo "assert failed: expected unknown project rm to fail" >&2
  exit 1
fi
assert_file_contains "$stderr7" "project not found"

echo "all tests passed"
