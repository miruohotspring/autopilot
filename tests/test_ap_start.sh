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
if [[ "${FAKE_AGENT_IGNORE_TERM:-}" == "1" ]]; then
  trap '' TERM HUP
fi
if [[ -n "${FAKE_AGENT_PID_FILE:-}" ]]; then
  printf '%s\n' "$$" > "${FAKE_AGENT_PID_FILE}"
fi
printf '%s\n' "argv:$*"
printf '%s\n' "${FAKE_AGENT_NAME:-agent}:${FAKE_AGENT_STDOUT:-fake agent completed}"
printf '%s\n' "${FAKE_AGENT_NAME:-agent}:${FAKE_AGENT_STDERR:-}" >&2
if [[ -n "${FAKE_AGENT_SLEEP_SECONDS:-}" ]]; then
  sleep "${FAKE_AGENT_SLEEP_SECONDS}"
fi
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

echo "[test] fails when project has no managed path"
HOME="$home2" "$AP_BIN" new PathlessProject pathless >/dev/null
stderr3c="$TMP_DIR/start_stderr3c.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start PathlessProject >/dev/null 2>"$stderr3c"
status3c=$?
set -e
if [[ "$status3c" -eq 0 ]]; then
  echo "assert failed: expected PathlessProject start to fail without a managed path" >&2
  exit 1
fi
assert_file_contains "$stderr3c" "ap start failed: no managed path"

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

# --- Phase 5 tests ---

echo "[test] run_counter increments and run_id uses counter format"
counter_repo="$TMP_DIR/repo-counter"
mkdir -p "$counter_repo"
new_project "CounterProject" "counter" "$counter_repo"
add_task "CounterProject" "counter task 1"
add_task "CounterProject" "counter task 2"
# Run first task
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start CounterProject >/dev/null
counter_state="$home2/.autopilot/projects/CounterProject/runtime/state/project.json"
assert_file_contains "$counter_state" "\"run_counter\": 1"
# run_id format: run-YYYYMMDD-HHMMSS-L01
counter_run1_dir="$(latest_run_dir "$home2/.autopilot/projects/CounterProject")"
counter_run1_id="$(basename "$counter_run1_dir")"
if ! echo "$counter_run1_id" | grep -qE '^run-[0-9]+-[0-9]+-L[0-9]+$'; then
  echo "assert failed: run_id '$counter_run1_id' does not match expected format" >&2
  exit 1
fi
# Run second task
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start CounterProject >/dev/null
assert_file_contains "$counter_state" "\"run_counter\": 2"

echo "[test] lock files are created during run and cleaned up after"
lock_repo="$TMP_DIR/repo-lock"
mkdir -p "$lock_repo"
new_project "LockProject" "lock" "$lock_repo"
add_task "LockProject" "lock task"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start LockProject >/dev/null
lock_dir="$home2/.autopilot/projects/LockProject/runtime/lock"
# Lock files should be cleaned up after successful run
if [[ -f "$lock_dir/project.lock" ]]; then
  echo "assert failed: project.lock should be deleted after run" >&2
  exit 1
fi
# lock.acquired and lock.released events should be recorded
assert_file_contains \
  "$home2/.autopilot/projects/LockProject/runtime/events/events.jsonl" \
  "\"type\": \"lock.acquired\""
assert_file_contains \
  "$home2/.autopilot/projects/LockProject/runtime/events/events.jsonl" \
  "\"type\": \"lock.released\""

echo "[test] live project lock blocks concurrent start"
cont_repo="$TMP_DIR/repo-concurrent"
mkdir -p "$cont_repo"
new_project "ConcurrentProject" "concurrent" "$cont_repo"
add_task "ConcurrentProject" "concurrent task"
cont_stdout="$TMP_DIR/start_concurrent_stdout.txt"
cont_stderr="$TMP_DIR/start_concurrent_stderr.txt"
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" FAKE_AGENT_SLEEP_SECONDS="2" \
  "$AP_BIN" start ConcurrentProject >"$cont_stdout" 2>"$cont_stderr" &
cont_pid=$!
cont_lock_dir="$home2/.autopilot/projects/ConcurrentProject/runtime/lock"
cont_project_state="$home2/.autopilot/projects/ConcurrentProject/runtime/state/project.json"
cont_task_state="$home2/.autopilot/projects/ConcurrentProject/runtime/state/tasks/concurrent-0001.json"
for _ in $(seq 1 100); do
  if [[ -f "$cont_lock_dir/project.lock" ]] && \
     [[ -f "$cont_task_state" ]] && \
     grep -q --fixed-strings '"status": "in_progress"' "$cont_task_state"; then
    break
  fi
  sleep 0.05
done
if [[ ! -f "$cont_lock_dir/project.lock" ]] || [[ ! -f "$cont_task_state" ]] || \
   ! grep -q --fixed-strings '"status": "in_progress"' "$cont_task_state"; then
  echo "assert failed: expected project.lock and in_progress task state while first start is running" >&2
  kill "$cont_pid" 2>/dev/null || true
  wait "$cont_pid" 2>/dev/null || true
  exit 1
fi
cont_second_stderr="$TMP_DIR/start_concurrent_second_stderr.txt"
set +e
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start ConcurrentProject >/dev/null 2>"$cont_second_stderr"
cont_second_status=$?
set -e
if [[ "$cont_second_status" -eq 0 ]]; then
  echo "assert failed: expected second ap start to fail while lock is held" >&2
  kill "$cont_pid" 2>/dev/null || true
  wait "$cont_pid" 2>/dev/null || true
  exit 1
fi
assert_file_contains "$cont_second_stderr" "project is already locked"
if [[ ! -f "$cont_lock_dir/project.lock" ]]; then
  echo "assert failed: project.lock disappeared while first start still held it" >&2
  kill "$cont_pid" 2>/dev/null || true
  wait "$cont_pid" 2>/dev/null || true
  exit 1
fi
assert_file_not_contains "$cont_project_state" "\"active_run_id\": null"
assert_file_contains "$cont_task_state" "\"status\": \"in_progress\""
assert_file_not_contains \
  "$home2/.autopilot/projects/ConcurrentProject/runtime/events/events.jsonl" \
  "\"reason\": \"stale_run_recovered\""
assert_file_not_contains \
  "$home2/.autopilot/projects/ConcurrentProject/runtime/events/events.jsonl" \
  "\"type\": \"run.interrupted\""
wait "$cont_pid"
assert_file_contains "$cont_stdout" "completed task: concurrent task"

echo "[test] killing parent ap process does not let a second start steal the live lock"
owner_repo="$TMP_DIR/repo-owner-lock"
mkdir -p "$owner_repo"
new_project "OwnerLockProject" "owner-lock" "$owner_repo"
add_task "OwnerLockProject" "owner lock task"
owner_pid_file="$TMP_DIR/owner-lock-agent.pid"
owner_stdout="$TMP_DIR/start_owner_stdout.txt"
owner_stderr="$TMP_DIR/start_owner_stderr.txt"
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" FAKE_AGENT_SLEEP_SECONDS="3" FAKE_AGENT_PID_FILE="$owner_pid_file" \
  "$AP_BIN" start OwnerLockProject >"$owner_stdout" 2>"$owner_stderr" &
owner_ap_pid=$!
owner_lock_dir="$home2/.autopilot/projects/OwnerLockProject/runtime/lock"
for _ in $(seq 1 100); do
  if [[ -f "$owner_lock_dir/project.lock" && -f "$owner_pid_file" ]]; then
    break
  fi
  sleep 0.05
done
if [[ ! -f "$owner_pid_file" ]]; then
  echo "assert failed: expected fake agent pid file for owner-lock test" >&2
  kill "$owner_ap_pid" 2>/dev/null || true
  wait "$owner_ap_pid" 2>/dev/null || true
  exit 1
fi
owner_agent_pid="$(cat "$owner_pid_file")"
kill "$owner_ap_pid" 2>/dev/null || true
sleep 0.2
if ! kill -0 "$owner_agent_pid" 2>/dev/null; then
  echo "assert failed: expected fake agent to keep running after parent ap was killed" >&2
  exit 1
fi
owner_second_stderr="$TMP_DIR/start_owner_second_stderr.txt"
set +e
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start OwnerLockProject >/dev/null 2>"$owner_second_stderr"
owner_second_status=$?
set -e
if [[ "$owner_second_status" -eq 0 ]]; then
  echo "assert failed: expected second ap start to fail while runner-owned lock is held" >&2
  exit 1
fi
assert_file_contains "$owner_second_stderr" "project is already locked"
assert_file_not_contains "$owner_second_stderr" "stale lock detected"
sleep 3.5

echo "[test] stale lock is detected and recovered"
stale_repo="$TMP_DIR/repo-stale"
mkdir -p "$stale_repo"
new_project "StaleProject" "stale" "$stale_repo"
add_task "StaleProject" "stale task"
# First run to set up state
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start StaleProject >/dev/null
# Manually create a stale project.lock with a non-existent PID
stale_lock_dir="$home2/.autopilot/projects/StaleProject/runtime/lock"
mkdir -p "$stale_lock_dir"
cat >"$stale_lock_dir/project.lock" <<EOF
{
  "pid": 99999999,
  "run_id": "run-20260101-000000-L0",
  "task_id": null,
  "started_at": "2026-01-01T00:00:00+00:00",
  "hostname": "test-host"
}
EOF
# Add a todo task (first one is done)
add_task "StaleProject" "task after stale"
# ap start should recover the stale lock and succeed
stale_stderr="$TMP_DIR/start_stale_stderr.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start StaleProject >/dev/null 2>"$stale_stderr"
assert_file_contains "$stale_stderr" "stale lock detected"
assert_file_contains \
  "$home2/.autopilot/projects/StaleProject/runtime/events/events.jsonl" \
  "\"type\": \"lock.stale_detected\""

echo "[test] old lock with a live reused pid is treated as stale after timeout margin"
aged_repo="$TMP_DIR/repo-aged-lock"
mkdir -p "$aged_repo"
new_project "AgedLockProject" "aged-lock" "$aged_repo"
add_task "AgedLockProject" "aged lock task"
aged_lock_dir="$home2/.autopilot/projects/AgedLockProject/runtime/lock"
mkdir -p "$aged_lock_dir"
cat >"$aged_lock_dir/project.lock" <<EOF
{
  "pid": $$,
  "run_id": "run-20260101-000000-L1",
  "task_id": null,
  "started_at": "2026-01-01T00:00:00+00:00",
  "hostname": "test-host"
}
EOF
touch -d '2 hours ago' "$aged_lock_dir/project.lock"
aged_stderr="$TMP_DIR/start_aged_lock_stderr.txt"
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start AgedLockProject >/dev/null 2>"$aged_stderr"
assert_file_contains "$aged_stderr" "stale lock detected"
assert_file_contains \
  "$home2/.autopilot/projects/AgedLockProject/runtime/events/events.jsonl" \
  "\"type\": \"lock.stale_detected\""

echo "[test] max_retries exhausted task is skipped with retry_exhausted event"
retry_repo="$TMP_DIR/repo-retry"
mkdir -p "$retry_repo"
new_project "RetryProject" "retry" "$retry_repo"
add_task "RetryProject" "exhausted task"
# Manually set the task state to failed with attempt_count >= max_retries
retry_tasks_dir="$home2/.autopilot/projects/RetryProject/runtime/state/tasks"
mkdir -p "$retry_tasks_dir"
# First run ap start once to create the task state file
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_AGENT_EXIT_CODE="1" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start RetryProject >/dev/null 2>/dev/null
set -e
# Now manually set attempt_count=3, max_retries=3 in the task state
retry_task_file="$(find "$retry_tasks_dir" -name "*.json" | head -1)"
perl -pi -e 's/"attempt_count": \d+/"attempt_count": 3/' "$retry_task_file"
perl -pi -e 's/"status": "[^"]*"/"status": "failed"/' "$retry_task_file"
# Add max_retries field if not present
if ! grep -q "max_retries" "$retry_task_file"; then
  perl -pi -e 's/"attempt_count": 3/"attempt_count": 3,\n  "max_retries": 3/' "$retry_task_file"
fi
# ap start should skip the exhausted task
retry_stderr="$TMP_DIR/start_retry_stderr.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start RetryProject 2>"$retry_stderr" >/dev/null
retry_status=$?
set -e
if [[ "$retry_status" -eq 0 ]]; then
  echo "assert failed: expected ap start to fail when all tasks are exhausted" >&2
  exit 1
fi
assert_file_contains "$retry_stderr" "max retries"
assert_file_contains \
  "$home2/.autopilot/projects/RetryProject/runtime/events/events.jsonl" \
  "\"type\": \"task.retry_exhausted\""

echo "[test] timeout detection uses watchdog marker instead of raw exit code"
timeout_repo="$TMP_DIR/repo-timeout"
mkdir -p "$timeout_repo"
new_project "TimeoutProject" "timeout" "$timeout_repo"
add_task "TimeoutProject" "timeout task"
timeout_stderr="$TMP_DIR/start_timeout_stderr.txt"
set +e
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" FAKE_AGENT_SLEEP_SECONDS="5" \
  "$AP_BIN" start TimeoutProject --timeout 1 2>"$timeout_stderr" >/dev/null
timeout_exit=$?
set -e
if [[ "$timeout_exit" -eq 0 ]]; then
  echo "assert failed: expected ap start to fail on timeout" >&2
  exit 1
fi
timeout_events="$home2/.autopilot/projects/TimeoutProject/runtime/events/events.jsonl"
assert_file_contains "$timeout_events" "\"type\": \"run.timeout\""
assert_file_contains "$timeout_stderr" "timed out after 1 seconds"
# Task should be in failed state
timeout_task="$(find "$home2/.autopilot/projects/TimeoutProject/runtime/state/tasks" -name "*.json" | head -1)"
assert_file_contains "$timeout_task" "\"status\": \"failed\""
assert_file_contains "$timeout_task" "\"last_run_exit_reason\": \"timeout\""
assert_file_contains "$timeout_task" "\"last_error\": \"timed out after 1s\""

echo "[test] timeout result is not retried when retry_on excludes timeout"
timeout_retry_repo="$TMP_DIR/repo-timeout-retry"
mkdir -p "$timeout_retry_repo"
new_project "TimeoutRetryProject" "timeout-retry" "$timeout_retry_repo"
add_task "TimeoutRetryProject" "timeout retry task"
set +e
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" FAKE_AGENT_SLEEP_SECONDS="5" \
  "$AP_BIN" start TimeoutRetryProject --timeout 1 >/dev/null 2>/dev/null
set -e
timeout_retry_task="$(find "$home2/.autopilot/projects/TimeoutRetryProject/runtime/state/tasks" -name "*.json" | head -1)"
timeout_retry_stderr="$TMP_DIR/start_timeout_retry_stderr.txt"
set +e
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start TimeoutRetryProject >/dev/null 2>"$timeout_retry_stderr"
timeout_retry_status=$?
set -e
if [[ "$timeout_retry_status" -eq 0 ]]; then
  echo "assert failed: expected timed out task to be excluded from retry by default" >&2
  exit 1
fi
assert_file_contains "$timeout_retry_stderr" "no runnable task"
assert_file_contains "$timeout_retry_task" "\"status\": \"failed\""
assert_file_contains "$timeout_retry_task" "\"attempt_count\": 1"
assert_file_contains "$timeout_retry_task" "\"last_run_exit_reason\": \"timeout\""

echo "[test] agent exit code 124 is not misclassified as timeout"
exit124_repo="$TMP_DIR/repo-exit124"
mkdir -p "$exit124_repo"
new_project "Exit124Project" "exit124" "$exit124_repo"
add_task "Exit124Project" "exit 124 task"
exit124_stderr="$TMP_DIR/start_exit124_stderr.txt"
set +e
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" FAKE_AGENT_EXIT_CODE="124" \
  "$AP_BIN" start Exit124Project >/dev/null 2>"$exit124_stderr"
exit124_status=$?
set -e
if [[ "$exit124_status" -eq 0 ]]; then
  echo "assert failed: expected ap start to fail on agent exit 124" >&2
  exit 1
fi
assert_file_contains "$exit124_stderr" "agent exited with status 124"
assert_file_not_contains "$exit124_stderr" "timed out"
exit124_events="$home2/.autopilot/projects/Exit124Project/runtime/events/events.jsonl"
assert_file_not_contains "$exit124_events" "\"type\": \"run.timeout\""
exit124_task="$(find "$home2/.autopilot/projects/Exit124Project/runtime/state/tasks" -name "*.json" | head -1)"
assert_file_contains "$exit124_task" "\"last_run_exit_reason\": \"failed\""
assert_file_contains "$exit124_task" "\"last_error\": \"agent exited with status 124\""

echo "[test] timeout kills the agent process before locks are released"
kill_timeout_repo="$TMP_DIR/repo-timeout-kill"
mkdir -p "$kill_timeout_repo"
new_project "KillTimeoutProject" "kill-timeout" "$kill_timeout_repo"
add_task "KillTimeoutProject" "timeout kill task"
kill_timeout_pid_file="$TMP_DIR/kill-timeout.pid"
kill_timeout_stderr="$TMP_DIR/start_kill_timeout_stderr.txt"
set +e
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" FAKE_AGENT_SLEEP_SECONDS="30" FAKE_AGENT_IGNORE_TERM="1" \
  FAKE_AGENT_PID_FILE="$kill_timeout_pid_file" \
  "$AP_BIN" start KillTimeoutProject --timeout 1 >/dev/null 2>"$kill_timeout_stderr"
kill_timeout_status=$?
set -e
if [[ "$kill_timeout_status" -eq 0 ]]; then
  echo "assert failed: expected ap start to fail on forced timeout" >&2
  exit 1
fi
kill_timeout_pid="$(cat "$kill_timeout_pid_file")"
sleep 0.2
if kill -0 "$kill_timeout_pid" 2>/dev/null; then
  echo "assert failed: agent pid $kill_timeout_pid should be terminated after timeout" >&2
  exit 1
fi
assert_file_contains "$kill_timeout_stderr" "timed out after 1 seconds"

echo "[test] direct start does not require external timeout command"
portable_repo="$TMP_DIR/repo-portable-timeout"
mkdir -p "$portable_repo"
new_project "PortableTimeoutProject" "portable-timeout" "$portable_repo"
add_task "PortableTimeoutProject" "portable timeout task"
fake_timeout_bin="$TMP_DIR/fake-timeout-bin"
mkdir -p "$fake_timeout_bin"
cat >"$fake_timeout_bin/timeout" <<'EOF'
#!/bin/bash
echo "unexpected timeout invocation" >&2
exit 99
EOF
chmod +x "$fake_timeout_bin/timeout"
portable_stdout="$TMP_DIR/start_portable_timeout_stdout.txt"
portable_stderr="$TMP_DIR/start_portable_timeout_stderr.txt"
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" \
  PATH="$fake_timeout_bin:$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start PortableTimeoutProject >"$portable_stdout" 2>"$portable_stderr"
assert_file_contains "$portable_stdout" "completed task: portable timeout task"
assert_file_not_contains "$portable_stderr" "unexpected timeout invocation"

echo "[test] corrupted lock file is treated as stale and recovered"
corrupt_repo="$TMP_DIR/repo-corrupt-lock"
mkdir -p "$corrupt_repo"
new_project "CorruptLockProject" "corrupt-lock" "$corrupt_repo"
add_task "CorruptLockProject" "corrupt lock task"
corrupt_lock_dir="$home2/.autopilot/projects/CorruptLockProject/runtime/lock"
mkdir -p "$corrupt_lock_dir"
cat >"$corrupt_lock_dir/project.lock" <<'EOF'
{"pid": {}}
EOF
corrupt_stderr="$TMP_DIR/start_corrupt_lock_stderr.txt"
env -u TMUX AUTOPILOT_START_DISABLE_TMUX=1 HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start CorruptLockProject >/dev/null 2>"$corrupt_stderr"
assert_file_contains "$corrupt_stderr" "stale lock detected"
assert_file_contains \
  "$home2/.autopilot/projects/CorruptLockProject/runtime/events/events.jsonl" \
  "\"type\": \"lock.stale_detected\""

echo "[test] active_run_id is set during run and cleared after"
active_repo="$TMP_DIR/repo-active"
mkdir -p "$active_repo"
new_project "ActiveProject" "active" "$active_repo"
add_task "ActiveProject" "active task"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start ActiveProject >/dev/null
active_state="$home2/.autopilot/projects/ActiveProject/runtime/state/project.json"
# After run, active_run_id should be null
assert_file_contains "$active_state" "\"active_run_id\": null"
# But last_run_id should be set
assert_file_not_contains "$active_state" "\"last_run_id\": null"

echo "[test] run.interrupted event is emitted for stale in_progress tasks"
interrupted_repo="$TMP_DIR/repo-interrupted"
mkdir -p "$interrupted_repo"
new_project "InterruptedProject" "interrupted" "$interrupted_repo"
add_task "InterruptedProject" "interrupted task"
# First run to create state
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_AGENT_EXIT_CODE="1" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start InterruptedProject >/dev/null 2>/dev/null
set -e
# Manually set status to in_progress (simulating a crash mid-run)
int_task="$(find "$home2/.autopilot/projects/InterruptedProject/runtime/state/tasks" -name "*.json" | head -1)"
perl -pi -e 's/"status": "[^"]*"/"status": "in_progress"/' "$int_task"
# Run ap start again - should detect interrupted run
interrupted_stdout="$TMP_DIR/start_interrupted_stdout.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" \
  FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start InterruptedProject >"$interrupted_stdout" 2>/dev/null
assert_file_contains \
  "$home2/.autopilot/projects/InterruptedProject/runtime/events/events.jsonl" \
  "\"type\": \"run.interrupted\""
assert_file_contains "$interrupted_stdout" "completed task: interrupted task"
assert_file_contains "$int_task" "\"status\": \"done\""

echo "all tests passed"
