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

write_fake_agent() {
  local dir="$1"
  local name="$2"
  mkdir -p "$dir"
  cat >"$dir/$name" <<'EOF'
#!/bin/bash
set -euo pipefail
printf '%s\n' "argv:$*"
printf '%s\n' "${FAKE_AGENT_NAME:-agent}:${FAKE_AGENT_STDOUT:-fake agent completed}"
printf '%s\n' "${FAKE_AGENT_NAME:-agent}:${FAKE_AGENT_STDERR:-}" >&2
if [[ -n "${FAKE_AGENT_MUTATE_TODO_FILE:-}" ]]; then
  perl -0pi -e 's/\Q$ENV{FAKE_AGENT_MUTATE_TODO_FROM}\E/$ENV{FAKE_AGENT_MUTATE_TODO_TO}/g' \
    "$FAKE_AGENT_MUTATE_TODO_FILE"
fi
exit "${FAKE_AGENT_EXIT_CODE:-0}"
EOF
  chmod +x "$dir/$name"
}

write_fake_tmux() {
  local dir="$1"
  cat >"$dir/tmux" <<'EOF'
#!/bin/bash
set -euo pipefail

STATE_DIR="${FAKE_TMUX_STATE_DIR:?}"
LOG_FILE="$STATE_DIR/tmux.log"
SESSION_FILE="$STATE_DIR/autopilot.session"

mkdir -p "$STATE_DIR/channels"

cmd="${1:-}"
shift || true
printf "%s %s\n" "$cmd" "$*" >>"$LOG_FILE"

case "$cmd" in
  -V)
    echo "tmux 3.2a"
    ;;
  has-session)
    [[ -f "$SESSION_FILE" ]]
    ;;
  new-session)
    touch "$SESSION_FILE"
    command="${*: -1}"
    if [[ "$command" != -* ]]; then
      bash -lc "$command" >/dev/null 2>&1 &
    fi
    ;;
  new-window)
    touch "$SESSION_FILE"
    command="${*: -1}"
    if [[ "$command" != -* ]]; then
      bash -lc "$command" >/dev/null 2>&1 &
    fi
    ;;
  wait-for)
    if [[ "${1:-}" == "-S" ]]; then
      touch "$STATE_DIR/channels/$2"
      exit 0
    fi
    channel="${1:?}"
    for _ in $(seq 1 500); do
      if [[ -f "$STATE_DIR/channels/$channel" ]]; then
        exit 0
      fi
      sleep 0.01
    done
    exit 1
    ;;
  attach-session)
    for _ in $(seq 1 500); do
      if compgen -G "$STATE_DIR/channels/*" >/dev/null; then
        exit 0
      fi
      sleep 0.01
    done
    exit 1
    ;;
  switch-client|kill-window)
    exit 0
    ;;
  *)
    echo "unexpected tmux command: $cmd" >&2
    exit 99
    ;;
esac
EOF
  chmod +x "$dir/tmux"
}

latest_run_dir() {
  local project_dir="$1"
  find "$project_dir/runtime/runs" -mindepth 1 -maxdepth 1 -type d | sort | tail -n 1
}

new_project() {
  local name="$1"
  local slug="$2"
  local repo_path="$3"
  HOME="$home2" "$AP_BIN" new "$name" "$slug" >/dev/null
  HOME="$home2" "$AP_BIN" add "$repo_path" -n main -p "$name" >/dev/null
}

add_task() {
  local project="$1"
  local title="$2"
  HOME="$home2" "$AP_BIN" task add "$title" -p "$project" >/dev/null
}

echo "[test] fails when ~/.autopilot does not exist"
home1="$TMP_DIR/home1"
mkdir -p "$home1"
stderr1="$TMP_DIR/start_stderr1.txt"
set +e
HOME="$home1" "$AP_BIN" start Demo 2>"$stderr1" >/dev/null
status1=$?
set -e
if [[ "$status1" -eq 0 ]]; then
  echo "assert failed: expected ap start to fail when ~/.autopilot is missing" >&2
  exit 1
fi
assert_file_contains "$stderr1" "Please run ap init first"

home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
fake_bin="$TMP_DIR/fake-bin"
write_fake_agent "$fake_bin" claude
write_fake_agent "$fake_bin" codex
write_fake_tmux "$fake_bin"
tmux_state="$TMP_DIR/tmux-state"
mkdir -p "$tmux_state"

echo "[test] supports interactive project selection and runs the selected ID-based task"
alpha_repo="$TMP_DIR/repo-alpha"
beta_repo="$TMP_DIR/repo-beta"
mkdir -p "$alpha_repo" "$beta_repo"
new_project "AlphaProject" "alpha" "$alpha_repo"
new_project "BetaProject" "beta" "$beta_repo"
add_task "AlphaProject" "first task"
add_task "AlphaProject" "second task"
add_task "BetaProject" "beta task"
stdout2="$TMP_DIR/start_stdout2.txt"
printf '2\n' | env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="beta summary" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start >"$stdout2"
assert_file_contains "$stdout2" "Select project to start:"
assert_file_contains "$stdout2" "Enter number to start:"
assert_file_contains "$stdout2" "completed task: beta task"
assert_file_contains "$home2/.autopilot/projects/BetaProject/TODO.md" "- [x] [beta-0001] beta task"
assert_file_contains "$tmux_state/tmux.log" "new-session -d -s autopilot -n start-BetaProject-"

echo "[test] creates runtime artifacts and uses slug-based task IDs"
stdout3="$TMP_DIR/start_stdout3.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="implemented first task" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start AlphaProject >"$stdout3"
assert_file_contains "$stdout3" "completed task: first task"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/TODO.md" "- [x] [alpha-0001] first task"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/TODO.md" "- [ ] [alpha-0002] second task"
alpha_run_dir="$(latest_run_dir "$home2/.autopilot/projects/AlphaProject")"
alpha_run_id="$(basename "$alpha_run_dir")"
assert_exists "$alpha_run_dir/meta.json"
assert_exists "$alpha_run_dir/result.json"
assert_exists "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/alpha-0001.json"
assert_exists "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/alpha-0002.json"
assert_file_contains "$alpha_run_dir/result.json" "\"task_id\": \"alpha-0001\""
assert_file_contains "$alpha_run_dir/result.json" "\"final_task_status\": \"done\""
assert_file_contains "$alpha_run_dir/result.json" "\"todo_update_applied\": true"
assert_file_contains "$alpha_run_dir/meta.json" "\"task_id\": \"alpha-0001\""
assert_file_contains "$alpha_run_dir/stdout.log" "implemented first task"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/alpha-0001.json" "\"status\": \"done\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/alpha-0001.json" "\"latest_run_id\": \"$alpha_run_id\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/alpha-0002.json" "\"status\": \"todo\""

echo "[test] pathless task add remains runnable after adding a non-main path"
pathless_repo="$TMP_DIR/repo-pathless"
mkdir -p "$pathless_repo"
HOME="$home2" "$AP_BIN" new PathlessProject pathless >/dev/null
HOME="$home2" "$AP_BIN" task add "pathless task" -p PathlessProject >/dev/null
HOME="$home2" "$AP_BIN" add "$pathless_repo" -n src -p PathlessProject >/dev/null
stdout3c="$TMP_DIR/start_stdout3c.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="pathless summary" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start PathlessProject >"$stdout3c"
assert_file_contains "$stdout3c" "completed task: pathless task"
assert_file_contains "$home2/.autopilot/projects/PathlessProject/runtime/state/tasks/pathless-0001.json" "\"related_paths\": []"
assert_file_contains "$home2/.autopilot/projects/PathlessProject/TODO.md" "- [x] [pathless-0001] pathless task"

echo "[test] marks tasks removed from TODO as not present"
cat >"$home2/.autopilot/projects/AlphaProject/TODO.md" <<'EOF'
# AlphaProject TODO

- [x] [alpha-0001] first task
EOF
stderr3b="$TMP_DIR/start_stderr3b.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start AlphaProject >/dev/null 2>"$stderr3b"
status3b=$?
set -e
if [[ "$status3b" -eq 0 ]]; then
  echo "assert failed: expected AlphaProject to have no runnable task" >&2
  exit 1
fi
assert_file_contains "$stderr3b" "ap start failed: no runnable task"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/alpha-0002.json" "\"present_in_todo\": false"

echo "[test] keeps TODO unchanged on failure and retries the same task ID"
failure_repo="$TMP_DIR/repo-failure"
mkdir -p "$failure_repo"
new_project "FailureProject" "failure" "$failure_repo"
add_task "FailureProject" "failing task"
stderr4="$TMP_DIR/start_stderr4.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_EXIT_CODE=9 FAKE_AGENT_STDOUT="partial log" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start FailureProject >/dev/null 2>"$stderr4"
status4=$?
set -e
if [[ "$status4" -eq 0 ]]; then
  echo "assert failed: expected agent failure to propagate" >&2
  exit 1
fi
assert_file_contains "$stderr4" "ap start failed: agent exited with status 9"
assert_file_contains "$home2/.autopilot/projects/FailureProject/TODO.md" "- [ ] [failure-0001] failing task"
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/tasks/failure-0001.json" "\"status\": \"failed\""
stdout4b="$TMP_DIR/start_stdout4b.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="fixed failure" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start FailureProject >"$stdout4b"
assert_file_contains "$stdout4b" "completed task: failing task"
assert_file_contains "$home2/.autopilot/projects/FailureProject/TODO.md" "- [x] [failure-0001] failing task"
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/tasks/failure-0001.json" "\"attempt_count\": 2"

echo "[test] title edits are treated as rename on the same task ID"
rename_repo="$TMP_DIR/repo-rename"
mkdir -p "$rename_repo"
new_project "RenameProject" "rename" "$rename_repo"
add_task "RenameProject" "old title"
cat >"$home2/.autopilot/projects/RenameProject/TODO.md" <<'EOF'
# RenameProject TODO

- [ ] [rename-0001] new title
EOF
stdout5="$TMP_DIR/start_stdout5.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="rename summary" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start RenameProject >"$stdout5"
assert_file_contains "$stdout5" "completed task: new title"
assert_file_contains "$home2/.autopilot/projects/RenameProject/runtime/state/tasks/rename-0001.json" "\"title\": \"new title\""
assert_file_contains "$home2/.autopilot/projects/RenameProject/runtime/events/events.jsonl" "\"type\": \"task.updated\""

echo "[test] reordering keeps task identity and changes selection order by TODO line"
reorder_repo="$TMP_DIR/repo-reorder"
mkdir -p "$reorder_repo"
new_project "ReorderProject" "reorder" "$reorder_repo"
add_task "ReorderProject" "first item"
add_task "ReorderProject" "second item"
cat >"$home2/.autopilot/projects/ReorderProject/TODO.md" <<'EOF'
# ReorderProject TODO

- [ ] [reorder-0002] second item

- [ ] [reorder-0001] first item
EOF
stdout6="$TMP_DIR/start_stdout6.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="reordered summary" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start ReorderProject >"$stdout6"
assert_file_contains "$stdout6" "completed task: second item"
assert_file_contains "$home2/.autopilot/projects/ReorderProject/runtime/state/tasks/reorder-0002.json" "\"source_line\": 3"
assert_file_contains "$home2/.autopilot/projects/ReorderProject/runtime/state/tasks/reorder-0001.json" "\"source_line\": 5"

echo "[test] fails fast on duplicate task IDs"
dup_repo="$TMP_DIR/repo-duplicate"
mkdir -p "$dup_repo"
new_project "DuplicateProject" "dup" "$dup_repo"
add_task "DuplicateProject" "first"
add_task "DuplicateProject" "second"
cat >"$home2/.autopilot/projects/DuplicateProject/TODO.md" <<'EOF'
# DuplicateProject TODO

- [ ] [dup-0001] first

- [ ] [dup-0001] second
EOF
stderr7="$TMP_DIR/start_stderr7.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start DuplicateProject >/dev/null 2>"$stderr7"
status7=$?
set -e
if [[ "$status7" -eq 0 ]]; then
  echo "assert failed: expected duplicate task IDs to fail" >&2
  exit 1
fi
assert_file_contains "$stderr7" "failed to parse TODO.md: duplicate task id dup-0001"
assert_file_contains "$home2/.autopilot/projects/DuplicateProject/runtime/events/events.jsonl" "\"type\": \"todo.sync_conflict\""

echo "[test] rejects invalid task ID format"
invalid_repo="$TMP_DIR/repo-invalid"
mkdir -p "$invalid_repo"
new_project "InvalidProject" "invalid" "$invalid_repo"
add_task "InvalidProject" "bad format task"
cat >"$home2/.autopilot/projects/InvalidProject/TODO.md" <<'EOF'
# InvalidProject TODO

- [ ] [Invalid-1] bad format task
EOF
stderr8="$TMP_DIR/start_stderr8.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start InvalidProject >/dev/null 2>"$stderr8"
status8=$?
set -e
if [[ "$status8" -eq 0 ]]; then
  echo "assert failed: expected invalid task ID format to fail" >&2
  exit 1
fi
assert_file_contains "$stderr8" "failed to parse TODO.md: invalid task id format [Invalid-1]"

echo "[test] rejects task IDs with the wrong slug prefix"
mismatch_repo="$TMP_DIR/repo-mismatch"
mkdir -p "$mismatch_repo"
new_project "MismatchProject" "mismatch" "$mismatch_repo"
add_task "MismatchProject" "wrong slug task"
cat >"$home2/.autopilot/projects/MismatchProject/TODO.md" <<'EOF'
# MismatchProject TODO

- [ ] [other-0001] wrong slug task
EOF
stderr9="$TMP_DIR/start_stderr9.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start MismatchProject >/dev/null 2>"$stderr9"
status9=$?
set -e
if [[ "$status9" -eq 0 ]]; then
  echo "assert failed: expected slug mismatch to fail" >&2
  exit 1
fi
assert_file_contains "$stderr9" "does not match project slug mismatch"

echo "[test] rejects unknown task IDs that do not exist in state"
unknown_repo="$TMP_DIR/repo-unknown"
mkdir -p "$unknown_repo"
new_project "UnknownProject" "unknown" "$unknown_repo"
add_task "UnknownProject" "known task"
cat >"$home2/.autopilot/projects/UnknownProject/TODO.md" <<'EOF'
# UnknownProject TODO

- [ ] [unknown-9999] known task
EOF
stderr10="$TMP_DIR/start_stderr10.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start UnknownProject >/dev/null 2>"$stderr10"
status10=$?
set -e
if [[ "$status10" -eq 0 ]]; then
  echo "assert failed: expected unknown task ID to fail" >&2
  exit 1
fi
assert_file_contains "$stderr10" "failed to sync TODO.md: task id unknown-9999 does not exist in state"

echo "[test] keeps task state done when TODO update conflicts after success"
conflict_repo="$TMP_DIR/repo-conflict"
mkdir -p "$conflict_repo"
new_project "ConflictProject" "conflict" "$conflict_repo"
add_task "ConflictProject" "conflict task"
stdout11="$TMP_DIR/start_stdout11.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="conflict summary" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  FAKE_AGENT_MUTATE_TODO_FILE="$home2/.autopilot/projects/ConflictProject/TODO.md" \
  FAKE_AGENT_MUTATE_TODO_FROM="- [ ] [conflict-0001] conflict task" \
  FAKE_AGENT_MUTATE_TODO_TO="- [ ] [conflict-0001] conflict task changed" \
  "$AP_BIN" start ConflictProject >"$stdout11"
conflict_run_dir="$(latest_run_dir "$home2/.autopilot/projects/ConflictProject")"
assert_file_contains "$stdout11" "completed task: conflict task"
assert_file_contains "$home2/.autopilot/projects/ConflictProject/TODO.md" "- [ ] [conflict-0001] conflict task changed"
assert_file_contains "$conflict_run_dir/result.json" "\"todo_update_applied\": false"
assert_file_contains "$home2/.autopilot/projects/ConflictProject/runtime/state/tasks/conflict-0001.json" "\"status\": \"done\""
assert_file_contains "$home2/.autopilot/projects/ConflictProject/runtime/events/events.jsonl" "\"type\": \"todo.sync_conflict\""

echo "[test] honors configured start agent"
config_repo="$TMP_DIR/repo-config"
mkdir -p "$config_repo"
new_project "ConfigProject" "config" "$config_repo"
add_task "ConfigProject" "config task"
cat >"$home2/.autopilot/config.toml" <<'EOF'
[start]
agent = "codex"
EOF
stdout12="$TMP_DIR/start_stdout12.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="configured agent run" \
  FAKE_AGENT_NAME="codex" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start ConfigProject >"$stdout12"
config_run_dir="$(latest_run_dir "$home2/.autopilot/projects/ConfigProject")"
assert_file_contains "$stdout12" "completed task: config task"
assert_file_contains "$config_run_dir/meta.json" "\"agent\": \"codex\""
assert_file_contains "$config_run_dir/stdout.log" "argv:exec --skip-git-repo-check --sandbox workspace-write --full-auto"

echo "all tests passed"
