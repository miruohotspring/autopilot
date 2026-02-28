#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap add`.
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

assert_project_has_name_path() {
  local file="$1"
  local project="$2"
  local name="$3"
  local path_value="$4"
  if ! awk -v project="$project" -v name="$name" -v path_value="$path_value" '
    $0 == project ":" { in_target = 1; next }
    in_target && $0 ~ /^[^ \t#][^:]*:/ { in_target = 0 }
    in_target && $0 == "    - name: \047" name "\047" {
      if (getline line <= 0) next
      if (line == "      path: \047" path_value "\047") {
        found = 1
      }
    }
    END { exit(found ? 0 : 1) }
  ' "$file"; then
    echo "assert failed: expected name/path '$name' -> '$path_value' in project '$project'" >&2
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
    in_target && $0 == "      path: \047" path_value "\047" { found = 1 }
    END { exit(found ? 0 : 1) }
  ' "$file"; then
    echo "assert failed: expected path '$path_value' not to be in project '$project'" >&2
    echo "--- file content ---" >&2
    cat "$file" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

assert_project_path_count() {
  local file="$1"
  local project="$2"
  local path_value="$3"
  local expected_count="$4"
  local actual_count
  actual_count="$(awk -v project="$project" -v path_value="$path_value" '
    $0 == project ":" { in_target = 1; next }
    in_target && $0 ~ /^[^ \t#][^:]*:/ { in_target = 0 }
    in_target && $0 == "      path: \047" path_value "\047" { count++ }
    END { print count + 0 }
  ' "$file")"
  if [[ "$actual_count" != "$expected_count" ]]; then
    echo "assert failed: expected path count $expected_count, got $actual_count" >&2
    echo "--- file content ---" >&2
    cat "$file" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

assert_symlink_target() {
  local path="$1"
  local expected_target="$2"
  if [[ ! -L "$path" ]]; then
    echo "assert failed: expected symlink: $path" >&2
    exit 1
  fi
  local actual_target
  actual_target="$(readlink -f "$path")"
  local expected_abs
  expected_abs="$(readlink -f "$expected_target")"
  if [[ "$actual_target" != "$expected_abs" ]]; then
    echo "assert failed: symlink target mismatch for $path" >&2
    echo "expected: $expected_abs" >&2
    echo "actual:   $actual_target" >&2
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
stderr1="$TMP_DIR/add_stderr1.txt"
set +e
HOME="$home1" "$AP_BIN" add . -n here -p ProjectA 2>"$stderr1" >/dev/null
status1=$?
set -e
if [[ "$status1" -eq 0 ]]; then
  echo "assert failed: expected ap add to fail when ~/.autopilot is missing" >&2
  exit 1
fi
assert_file_contains "$stderr1" "Please run ap init first"

# Prepare reusable HOME and projects.
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
HOME="$home2" "$AP_BIN" new AlphaProject >/dev/null
HOME="$home2" "$AP_BIN" new BetaProject >/dev/null
projects_file="$home2/.autopilot/projects.yaml"

# Case 2:
# Relative path should be normalized, stored with name, and symlinked.
echo "[test] normalizes path, stores name/path pair, and creates symlink"
mkdir -p "$TMP_DIR/work/rel_here"
stdout2="$TMP_DIR/add_stdout2.txt"
(
  cd "$TMP_DIR/work/rel_here"
  HOME="$home2" "$AP_BIN" add . -n rel-root -p AlphaProject >"$stdout2"
)
expected_abs_rel="$(cd "$TMP_DIR/work/rel_here" && pwd -P)"
assert_file_contains "$stdout2" "added path: $expected_abs_rel"
assert_project_has_name_path "$projects_file" "AlphaProject" "rel-root" "$expected_abs_rel"
assert_symlink_target "$home2/.autopilot/projects/AlphaProject/rel-root" "$expected_abs_rel"

# Case 3:
# Duplicate path+name should be no-op and not an error.
echo "[test] does nothing when path+name already exists"
stdout3="$TMP_DIR/add_stdout3.txt"
(
  cd "$TMP_DIR/work/rel_here"
  HOME="$home2" "$AP_BIN" add . -n rel-root -p AlphaProject >"$stdout3"
)
assert_file_contains "$stdout3" "path already exists: $expected_abs_rel"
assert_project_path_count "$projects_file" "AlphaProject" "$expected_abs_rel" "1"

# Case 4:
# Without -p, command should prompt for project selection and not ask y/n confirmation.
echo "[test] supports interactive project selection without confirmation"
mkdir -p "$TMP_DIR/work/interactive_target"
interactive_path="$(cd "$TMP_DIR/work/interactive_target" && pwd -P)"
stdout4="$TMP_DIR/add_stdout4.txt"
printf '2\n' | HOME="$home2" "$AP_BIN" add "$interactive_path" -n interactive >"$stdout4"
assert_file_contains "$stdout4" "Select project to add path:"
assert_file_contains "$stdout4" "Enter number to add path:"
assert_file_not_contains "$stdout4" "[y/n]"
assert_project_has_name_path "$projects_file" "BetaProject" "interactive" "$interactive_path"
assert_project_not_has_path "$projects_file" "AlphaProject" "$interactive_path"
assert_symlink_target "$home2/.autopilot/projects/BetaProject/interactive" "$interactive_path"

# Case 5:
# Unknown and invalid project names should fail.
echo "[test] validates target project name and existence"
run_expect_fail env HOME="$home2" "$AP_BIN" add . -n valid -p "-bad"
stderr5="$TMP_DIR/add_stderr5.txt"
set +e
HOME="$home2" "$AP_BIN" add . -n valid -p NotExists 2>"$stderr5" >/dev/null
status5=$?
set -e
if [[ "$status5" -eq 0 ]]; then
  echo "assert failed: expected ap add to fail for unknown project" >&2
  exit 1
fi
assert_file_contains "$stderr5" "project not found"

# Case 6:
# Without -n, empty input should default to main.
echo "[test] defaults to main when path name input is empty"
mkdir -p "$TMP_DIR/work/default_main"
main_path="$(cd "$TMP_DIR/work/default_main" && pwd -P)"
stdout6="$TMP_DIR/add_stdout6.txt"
printf '\n' | HOME="$home2" "$AP_BIN" add "$main_path" -p AlphaProject >"$stdout6"
assert_file_contains "$stdout6" "Enter path name [main]:"
assert_project_has_name_path "$projects_file" "AlphaProject" "main" "$main_path"
assert_symlink_target "$home2/.autopilot/projects/AlphaProject/main" "$main_path"

# Case 7:
# If main already exists, empty input should fail and explicit name should be required.
echo "[test] requires explicit input when main already exists"
mkdir -p "$TMP_DIR/work/main_conflict"
main_conflict_path="$(cd "$TMP_DIR/work/main_conflict" && pwd -P)"
stderr7="$TMP_DIR/add_stderr7.txt"
set +e
printf '\n' | HOME="$home2" "$AP_BIN" add "$main_conflict_path" -p AlphaProject 2>"$stderr7" >/dev/null
status7=$?
set -e
if [[ "$status7" -eq 0 ]]; then
  echo "assert failed: expected ap add to fail when main already exists and input is empty" >&2
  exit 1
fi
assert_file_contains "$stderr7" "path name is required because 'main' already exists"
assert_project_not_has_path "$projects_file" "AlphaProject" "$main_conflict_path"

# Case 8:
# Path name must be valid as a path basename.
echo "[test] validates path name format"
run_expect_fail env HOME="$home2" "$AP_BIN" add . -n "bad/name" -p AlphaProject >/dev/null 2>&1

echo "all tests passed"
