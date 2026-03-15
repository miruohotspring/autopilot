#!/usr/bin/env bash
set -euo pipefail

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
  if ! grep -q --fixed-strings -- "$expected" "$path"; then
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
  if grep -q --fixed-strings -- "$expected" "$path"; then
    echo "assert failed: expected '$expected' not to appear in $path" >&2
    echo "--- file content ---" >&2
    cat "$path" >&2
    echo "--------------------" >&2
    exit 1
  fi
}

assert_exists() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    echo "assert failed: expected to exist: $path" >&2
    exit 1
  fi
}

assert_not_exists() {
  local path="$1"
  if [[ -e "$path" ]]; then
    echo "assert failed: expected not to exist: $path" >&2
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

echo "[test] fails when ~/.autopilot does not exist"
home1="$TMP_DIR/home1"
mkdir -p "$home1"
stderr1="$TMP_DIR/task_add_stderr1.txt"
set +e
HOME="$home1" "$AP_BIN" task add "Task A" -p Demo 2>"$stderr1" >/dev/null
status1=$?
set -e
if [[ "$status1" -eq 0 ]]; then
  echo "assert failed: expected ap task add to fail when ~/.autopilot is missing" >&2
  exit 1
fi
assert_file_contains "$stderr1" "Please run ap init first"

echo "[test] adds slug-based task IDs to TODO.md and state"
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
HOME="$home2" "$AP_BIN" new Demo demo >/dev/null
stdout2="$TMP_DIR/task_add_stdout2.txt"
HOME="$home2" "$AP_BIN" task add "First task" -p Demo >"$stdout2"
assert_file_contains "$stdout2" "added task: [demo-0001] First task"
assert_file_contains "$home2/.autopilot/projects/Demo/TODO.md" "- [ ] [demo-0001] First task"
assert_exists "$home2/.autopilot/projects/Demo/runtime/state/tasks/demo-0001.json"
assert_file_contains "$home2/.autopilot/projects/Demo/runtime/state/tasks/demo-0001.json" "\"id\": \"demo-0001\""
assert_file_contains "$home2/.autopilot/projects/Demo/runtime/state/tasks/demo-0001.json" "\"title\": \"First task\""
assert_file_contains "$home2/.autopilot/projects/Demo/runtime/state/tasks/demo-0001.json" "\"generated_by\": \"ap.task_add\""
assert_file_contains "$home2/.autopilot/projects/Demo/runtime/state/tasks/demo-0001.json" "\"related_paths\": []"

echo "[test] increments IDs and supports interactive project selection"
HOME="$home2" "$AP_BIN" new Other other >/dev/null
stdout3="$TMP_DIR/task_add_stdout3.txt"
printf '2\n' | HOME="$home2" "$AP_BIN" task add "Other task" >"$stdout3"
assert_file_contains "$stdout3" "Select project to add task:"
assert_file_contains "$stdout3" "Enter number to add task:"
assert_file_contains "$stdout3" "added task: [other-0001] Other task"
HOME="$home2" "$AP_BIN" task add "Second task" -p Demo >/dev/null
assert_file_contains "$home2/.autopilot/projects/Demo/TODO.md" "- [ ] [demo-0002] Second task"
assert_exists "$home2/.autopilot/projects/Demo/runtime/state/tasks/demo-0002.json"

echo "[test] validates project existence and non-empty titles"
run_expect_fail env HOME="$home2" "$AP_BIN" task add "" -p Demo >/dev/null 2>&1
stderr4="$TMP_DIR/task_add_stderr4.txt"
set +e
HOME="$home2" "$AP_BIN" task add "Missing project task" -p Missing 2>"$stderr4" >/dev/null
status4=$?
set -e
if [[ "$status4" -eq 0 ]]; then
  echo "assert failed: expected ap task add to fail for unknown project" >&2
  exit 1
fi
assert_file_contains "$stderr4" "project not found"

echo "[test] fails fast when TODO.md references an unknown task ID"
cat >"$home2/.autopilot/projects/Demo/TODO.md" <<'EOF'
# Demo TODO

- [ ] [demo-0009] ghost
EOF
stderr5="$TMP_DIR/task_add_stderr5.txt"
set +e
HOME="$home2" "$AP_BIN" task add "Should not add" -p Demo 2>"$stderr5" >/dev/null
status5=$?
set -e
if [[ "$status5" -eq 0 ]]; then
  echo "assert failed: expected ap task add to fail for unknown TODO task IDs" >&2
  exit 1
fi
assert_file_contains "$stderr5" "failed to sync TODO.md: task id demo-0009 does not exist in state"
assert_file_not_contains "$home2/.autopilot/projects/Demo/TODO.md" "Should not add"
assert_not_exists "$home2/.autopilot/projects/Demo/runtime/state/tasks/demo-0010.json"

echo "[test] leaves TODO.md clean when state staging fails"
home3="$TMP_DIR/home3"
mkdir -p "$home3/.autopilot"
HOME="$home3" "$AP_BIN" new Broken broken >/dev/null
mkdir -p "$home3/.autopilot/projects/Broken/runtime/state/tasks"
chmod 500 "$home3/.autopilot/projects/Broken/runtime/state/tasks"
stderr6="$TMP_DIR/task_add_stderr6.txt"
set +e
HOME="$home3" "$AP_BIN" task add "Will fail" -p Broken 2>"$stderr6" >/dev/null
status6=$?
set -e
chmod 700 "$home3/.autopilot/projects/Broken/runtime/state/tasks"
if [[ "$status6" -eq 0 ]]; then
  echo "assert failed: expected ap task add to fail when task state staging cannot be written" >&2
  exit 1
fi
assert_file_contains "$stderr6" "ap task add failed:"
assert_file_not_contains "$home3/.autopilot/projects/Broken/TODO.md" "Will fail"
assert_not_exists "$home3/.autopilot/projects/Broken/runtime/state/tasks/broken-0001.json"

echo "all tests passed"
