#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap start`.
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

# Case 1:
# Without ~/.autopilot, command should fail with guidance.
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

# Prepare reusable HOME and fake agent.
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot"
fake_bin="$TMP_DIR/fake-bin"
write_fake_agent "$fake_bin" claude
write_fake_agent "$fake_bin" codex
write_fake_tmux "$fake_bin"
tmux_state="$TMP_DIR/tmux-state"
mkdir -p "$tmux_state"
project_repo_a="$TMP_DIR/repo-a"
project_repo_b="$TMP_DIR/repo-b"
mkdir -p "$project_repo_a" "$project_repo_b"
HOME="$home2" "$AP_BIN" new AlphaProject >/dev/null
HOME="$home2" "$AP_BIN" new BetaProject >/dev/null
HOME="$home2" "$AP_BIN" add "$project_repo_a" -n main -p AlphaProject >/dev/null
HOME="$home2" "$AP_BIN" add "$project_repo_b" -n main -p BetaProject >/dev/null

cat >"$home2/.autopilot/projects/AlphaProject/TODO.md" <<'EOF'
# AlphaProject TODO

- [ ] first task
- [ ] second task
EOF

cat >"$home2/.autopilot/projects/BetaProject/TODO.md" <<'EOF'
# BetaProject TODO

- [ ] beta task
EOF

# Case 2:
# Interactive project selection should work when project name is omitted.
echo "[test] supports interactive project selection"
stdout2="$TMP_DIR/start_stdout2.txt"
printf '2\n' | env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="beta summary" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start >"$stdout2"
assert_file_contains "$stdout2" "Select project to start:"
assert_file_contains "$stdout2" "Enter number to start:"
assert_file_contains "$stdout2" "completed task: beta task"
assert_file_contains "$home2/.autopilot/projects/BetaProject/TODO.md" "- [x] beta task"
assert_file_contains "$tmux_state/tmux.log" "new-session -d -s autopilot -n start-BetaProject-"
assert_file_contains "$tmux_state/tmux.log" "attach-session -t autopilot:start-BetaProject-"

# Reset BetaProject TODO because project ordering is lexical and project output text checks are simpler below.
cat >"$home2/.autopilot/projects/BetaProject/TODO.md" <<'EOF'
# BetaProject TODO

- [ ] beta task
EOF

# Case 3:
# Success should create runtime artifacts and mark the first open task done.
echo "[test] creates runtime artifacts and marks first task done on success"
stdout3="$TMP_DIR/start_stdout3.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="implemented first task" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start AlphaProject >"$stdout3"
assert_file_contains "$stdout3" "completed task: first task"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/TODO.md" "- [x] first task"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/TODO.md" "- [ ] second task"
alpha_run_dir="$(latest_run_dir "$home2/.autopilot/projects/AlphaProject")"
alpha_run_id="$(basename "$alpha_run_dir")"
assert_exists "$alpha_run_dir/meta.json"
assert_exists "$alpha_run_dir/prompt.txt"
assert_exists "$alpha_run_dir/stdout.log"
assert_exists "$alpha_run_dir/stderr.log"
assert_exists "$alpha_run_dir/result.json"
assert_exists "$home2/.autopilot/projects/AlphaProject/runtime/state/project.json"
assert_exists "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/task-0001.json"
assert_exists "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/task-0002.json"
assert_exists "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl"
assert_file_contains "$alpha_run_dir/result.json" "\"status\": \"succeeded\""
assert_file_contains "$alpha_run_dir/result.json" "\"task_id\": \"task-0001\""
assert_file_contains "$alpha_run_dir/result.json" "\"attempt_number\": 1"
assert_file_contains "$alpha_run_dir/result.json" "\"final_task_status\": \"done\""
assert_file_contains "$alpha_run_dir/result.json" "\"todo_update_applied\": true"
assert_file_contains "$alpha_run_dir/result.json" "implemented first task"
assert_file_contains "$alpha_run_dir/meta.json" "\"task_id\": \"task-0001\""
assert_file_contains "$alpha_run_dir/meta.json" "\"attempt_number\": 1"
assert_file_contains "$alpha_run_dir/stdout.log" "argv:-p --dangerously-skip-permissions"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/last_run.json" "\"status\": \"succeeded\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/task-0001.json" "\"status\": \"done\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/task-0001.json" "\"attempt_count\": 1"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/task-0001.json" "\"latest_run_id\": \"$alpha_run_id\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/task-0002.json" "\"status\": \"todo\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/task-0002.json" "\"present_in_todo\": true"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/project.json" "\"last_run_id\": \"$alpha_run_id\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/project.json" "\"todo\": 1"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/project.json" "\"done\": 1"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"type\": \"task.discovered\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"type\": \"task.selected\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"type\": \"run.started\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"type\": \"run.stdout\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"type\": \"run.stderr\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"type\": \"run.finished\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"type\": \"result.final\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"stream\": \"stdout\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"stream\": \"stderr\""
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"sequence\": 1"
assert_file_contains "$tmux_state/tmux.log" "new-window -d -t autopilot -n start-AlphaProject-"
assert_file_contains "$stdout3" "started task window: autopilot:start-AlphaProject-"
assert_file_contains "$alpha_run_dir/result.json" "\"process_status\": \"succeeded\""
assert_file_contains "$alpha_run_dir/result.json" "\"process_exit_code\": 0"
assert_file_contains "$alpha_run_dir/result.json" "\"alert_id\": null"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/events/events.jsonl" "\"actor\": \"agent.claude\""

# Case 3b:
# Tasks removed from TODO should remain in state but be excluded from selection.
echo "[test] marks removed tasks as not present in TODO"
cat >"$home2/.autopilot/projects/AlphaProject/TODO.md" <<'EOF'
# AlphaProject TODO

- [x] first task
EOF
stderr3b="$TMP_DIR/start_stderr3b.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start AlphaProject > /dev/null 2>"$stderr3b"
status3b=$?
set -e
if [[ "$status3b" -eq 0 ]]; then
  echo "assert failed: expected AlphaProject to have no runnable task after removing second task" >&2
  exit 1
fi
assert_file_contains "$stderr3b" "ap start failed: no runnable task found in TODO.md"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/state/tasks/task-0002.json" "\"present_in_todo\": false"

# Case 4:
# Failure should keep TODO unchanged and still persist artifacts.
echo "[test] keeps TODO unchanged and preserves artifacts on agent failure"
failure_repo="$TMP_DIR/repo-failure"
mkdir -p "$failure_repo"
HOME="$home2" "$AP_BIN" new FailureProject >/dev/null
HOME="$home2" "$AP_BIN" add "$failure_repo" -n main -p FailureProject >/dev/null
cat >"$home2/.autopilot/projects/FailureProject/TODO.md" <<'EOF'
# FailureProject TODO

- [ ] failing task
EOF
stderr4="$TMP_DIR/start_stderr4.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_EXIT_CODE=9 FAKE_AGENT_STDOUT="partial log" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start FailureProject > /dev/null 2>"$stderr4"
status4=$?
set -e
if [[ "$status4" -eq 0 ]]; then
  echo "assert failed: expected agent failure to propagate" >&2
  exit 1
fi
assert_file_contains "$stderr4" "ap start failed: agent exited with status 9"
assert_file_contains "$home2/.autopilot/projects/FailureProject/TODO.md" "- [ ] failing task"
failure_run_dir="$(latest_run_dir "$home2/.autopilot/projects/FailureProject")"
failure_run_id="$(basename "$failure_run_dir")"
assert_exists "$failure_run_dir/result.json"
assert_file_contains "$failure_run_dir/result.json" "\"status\": \"failed\""
assert_file_contains "$failure_run_dir/result.json" "\"process_status\": \"failed\""
assert_file_contains "$failure_run_dir/result.json" "\"process_exit_code\": 9"
assert_file_contains "$failure_run_dir/result.json" "\"task_id\": \"task-0001\""
assert_file_contains "$failure_run_dir/result.json" "\"attempt_number\": 1"
assert_file_contains "$failure_run_dir/result.json" "\"final_task_status\": \"failed\""
assert_file_contains "$failure_run_dir/result.json" "\"todo_update_applied\": false"
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/events/events.jsonl" "\"type\": \"result.final\""
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/events/events.jsonl" "\"final_task_status\": \"failed\""
assert_file_contains "$failure_run_dir/stdout.log" "partial log"
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/tasks/task-0001.json" "\"status\": \"failed\""
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/tasks/task-0001.json" "\"attempt_count\": 1"
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/tasks/task-0001.json" "\"latest_run_id\": \"$failure_run_id\""
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/tasks/task-0001.json" "\"last_error\": \"agent exited with status 9\""
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/project.json" "\"last_run_id\": \"$failure_run_id\""
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/project.json" "\"failed\": 1"

# Case 4b:
# Failed tasks should be selectable again and increment attempt_count.
echo "[test] retries failed tasks using persisted task state"
sleep 1
stdout4b="$TMP_DIR/start_stdout4b.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="fixed failure" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start FailureProject >"$stdout4b"
assert_file_contains "$stdout4b" "completed task: failing task"
failure_retry_run_dir="$(latest_run_dir "$home2/.autopilot/projects/FailureProject")"
failure_retry_run_id="$(basename "$failure_retry_run_dir")"
assert_file_contains "$home2/.autopilot/projects/FailureProject/TODO.md" "- [x] failing task"
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/tasks/task-0001.json" "\"status\": \"done\""
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/tasks/task-0001.json" "\"attempt_count\": 2"
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/tasks/task-0001.json" "\"latest_run_id\": \"$failure_retry_run_id\""
assert_file_contains "$home2/.autopilot/projects/FailureProject/runtime/state/project.json" "\"done\": 1"
assert_file_not_contains "$home2/.autopilot/projects/FailureProject/runtime/state/project.json" "\"failed\": 1"

# Case 4c:
# Blocked tasks should stay open, create alerts when approval is required, and be rerunnable.
echo "[test] records blocked runs and creates alerts"
blocked_repo="$TMP_DIR/blocked-repo"
mkdir -p "$blocked_repo"
HOME="$home2" "$AP_BIN" new BlockedProject >/dev/null
HOME="$home2" "$AP_BIN" add "$blocked_repo" -n main -p BlockedProject >/dev/null
cat >"$home2/.autopilot/projects/BlockedProject/TODO.md" <<'EOF'
# BlockedProject TODO

- [ ] blocked task
EOF
blocked_events="$home2/.autopilot/projects/BlockedProject/runtime/events/events.jsonl"
stderr4c="$TMP_DIR/start_stderr4c.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" \
  FAKE_AGENT_STDOUT="AUTOPILOT_APPROVAL_REQUIRED: production migration approval needed" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start BlockedProject > /dev/null 2>"$stderr4c"
status4c=$?
set -e
if [[ "$status4c" -eq 0 ]]; then
  echo "assert failed: expected blocked run to return non-zero" >&2
  exit 1
fi
assert_file_contains "$stderr4c" "ap start blocked: production migration approval needed"
assert_file_contains "$home2/.autopilot/projects/BlockedProject/TODO.md" "- [ ] blocked task"
assert_file_contains "$home2/.autopilot/projects/BlockedProject/runtime/state/tasks/task-0001.json" "\"status\": \"blocked\""
assert_file_contains "$home2/.autopilot/projects/BlockedProject/runtime/state/tasks/task-0001.json" "\"last_error\": \"production migration approval needed\""
assert_file_contains "$home2/.autopilot/projects/BlockedProject/runtime/state/project.json" "\"blocked\": 1"
assert_exists "$home2/.autopilot/projects/BlockedProject/runtime/alerts/alert-0001.json"
assert_file_contains "$home2/.autopilot/projects/BlockedProject/runtime/alerts/alert-0001.json" "\"type\": \"approval_required\""
assert_file_contains "$home2/.autopilot/projects/BlockedProject/runtime/alerts/alert-0001.json" "\"message\": \"production migration approval needed\""
assert_file_contains "$blocked_events" "\"type\": \"task.blocked\""
assert_file_contains "$blocked_events" "\"type\": \"alert.created\""
assert_file_contains "$blocked_events" "\"type\": \"result.final\""
assert_file_contains "$blocked_events" "\"final_task_status\": \"blocked\""
assert_file_contains "$blocked_events" "\"alert_id\": \"alert-0001\""
blocked_lines_before="$(wc -l <"$blocked_events")"
sleep 1
stdout4c_retry="$TMP_DIR/start_stdout4c_retry.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="approval received" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start BlockedProject >"$stdout4c_retry"
blocked_lines_after="$(wc -l <"$blocked_events")"
if (( blocked_lines_after <= blocked_lines_before )); then
  echo "assert failed: expected blocked project event log to grow append-only" >&2
  exit 1
fi
assert_file_contains "$stdout4c_retry" "completed task: blocked task"
assert_file_contains "$home2/.autopilot/projects/BlockedProject/TODO.md" "- [x] blocked task"
assert_file_contains "$home2/.autopilot/projects/BlockedProject/runtime/state/tasks/task-0001.json" "\"status\": \"done\""
assert_file_contains "$home2/.autopilot/projects/BlockedProject/runtime/state/tasks/task-0001.json" "\"attempt_count\": 2"
assert_file_contains "$blocked_events" "\"from\": \"blocked\", \"to\": \"in_progress\""


# Case 5:
# Multiple paths without main should fail.
echo "[test] fails when project has multiple paths and no main"
multi_repo_a="$TMP_DIR/multi-a"
multi_repo_b="$TMP_DIR/multi-b"
mkdir -p "$multi_repo_a" "$multi_repo_b"
HOME="$home2" "$AP_BIN" new MultiProject >/dev/null
HOME="$home2" "$AP_BIN" add "$multi_repo_a" -n api -p MultiProject >/dev/null
HOME="$home2" "$AP_BIN" add "$multi_repo_b" -n web -p MultiProject >/dev/null
cat >"$home2/.autopilot/projects/MultiProject/TODO.md" <<'EOF'
# MultiProject TODO

- [ ] multi task
EOF
stderr5="$TMP_DIR/start_stderr5.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start MultiProject 2>"$stderr5" >/dev/null
status5=$?
set -e
if [[ "$status5" -eq 0 ]]; then
  echo "assert failed: expected multi-path project without main to fail" >&2
  exit 1
fi
assert_file_contains "$stderr5" "ap start failed: project has multiple paths and no 'main'"

# Case 6:
# Missing runnable task should fail.
echo "[test] fails when no runnable task exists"
empty_repo="$TMP_DIR/empty-repo"
mkdir -p "$empty_repo"
HOME="$home2" "$AP_BIN" new EmptyProject >/dev/null
HOME="$home2" "$AP_BIN" add "$empty_repo" -n main -p EmptyProject >/dev/null
cat >"$home2/.autopilot/projects/EmptyProject/TODO.md" <<'EOF'
# EmptyProject TODO

- [x] already done
EOF
stderr6="$TMP_DIR/start_stderr6.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start EmptyProject 2>"$stderr6" >/dev/null
status6=$?
set -e
if [[ "$status6" -eq 0 ]]; then
  echo "assert failed: expected no-task project to fail" >&2
  exit 1
fi
assert_file_contains "$stderr6" "ap start failed: no runnable task found in TODO.md"
assert_exists "$home2/.autopilot/projects/EmptyProject/runtime/state/project.json"
assert_exists "$home2/.autopilot/projects/EmptyProject/runtime/state/tasks/task-0001.json"
assert_exists "$home2/.autopilot/projects/EmptyProject/runtime/events/events.jsonl"
assert_file_contains "$home2/.autopilot/projects/EmptyProject/runtime/state/tasks/task-0001.json" "\"status\": \"done\""
assert_file_contains "$home2/.autopilot/projects/EmptyProject/runtime/state/project.json" "\"done\": 1"

# Case 7:
# config.toml should control which agent CLI is used.
echo "[test] honors configured start agent from config.toml"
config_repo="$TMP_DIR/config-repo"
mkdir -p "$config_repo"
HOME="$home2" "$AP_BIN" new ConfigProject >/dev/null
HOME="$home2" "$AP_BIN" add "$config_repo" -n main -p ConfigProject >/dev/null
cat >"$home2/.autopilot/projects/ConfigProject/TODO.md" <<'EOF'
# ConfigProject TODO

- [ ] config task
EOF
cat >"$home2/.autopilot/config.toml" <<'EOF'
[start]
agent = "codex"
EOF
stdout7="$TMP_DIR/start_stdout7.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="configured agent run" \
  FAKE_AGENT_NAME="codex" FAKE_TMUX_STATE_DIR="$tmux_state" "$AP_BIN" start ConfigProject >"$stdout7"
config_run_dir="$(latest_run_dir "$home2/.autopilot/projects/ConfigProject")"
assert_file_contains "$stdout7" "completed task: config task"
assert_file_contains "$config_run_dir/meta.json" "\"agent\": \"codex\""
assert_file_contains "$config_run_dir/stdout.log" "argv:exec --skip-git-repo-check --sandbox workspace-write --full-auto"
assert_file_contains "$config_run_dir/stdout.log" "codex:configured agent run"

# Case 8:
# Missing configured agent should fail even if another supported agent exists.
echo "[test] fails when configured agent CLI is missing"
cat >"$home2/.autopilot/config.toml" <<'EOF'
[start]
agent = "codex"
EOF
missing_repo="$TMP_DIR/missing-agent-repo"
mkdir -p "$missing_repo"
HOME="$home2" "$AP_BIN" new MissingAgentProject >/dev/null
HOME="$home2" "$AP_BIN" add "$missing_repo" -n main -p MissingAgentProject >/dev/null
cat >"$home2/.autopilot/projects/MissingAgentProject/TODO.md" <<'EOF'
# MissingAgentProject TODO

- [ ] missing agent task
EOF
fake_claude_only="$TMP_DIR/fake-claude-only"
write_fake_agent "$fake_claude_only" claude
stderr8="$TMP_DIR/start_stderr8.txt"
set +e
write_fake_tmux "$fake_claude_only"
missing_tmux_state="$TMP_DIR/missing-tmux-state"
mkdir -p "$missing_tmux_state"
env -u TMUX HOME="$home2" PATH="$fake_claude_only" FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$missing_tmux_state" \
  "$AP_BIN" start MissingAgentProject 2>"$stderr8" >/dev/null
status8=$?
set -e
if [[ "$status8" -eq 0 ]]; then
  echo "assert failed: expected missing configured agent to fail" >&2
  exit 1
fi
assert_file_contains "$stderr8" "ap start failed: configured agent CLI not found: codex"

# Case 9:
# Reopening a completed TODO item should move state back through todo and increment attempts on rerun.
echo "[test] syncs reopened TODO items back to todo"
cat >"$home2/.autopilot/config.toml" <<'EOF'
[start]
agent = "claude"
EOF
reopen_repo="$TMP_DIR/reopen-repo"
mkdir -p "$reopen_repo"
HOME="$home2" "$AP_BIN" new ReopenProject >/dev/null
HOME="$home2" "$AP_BIN" add "$reopen_repo" -n main -p ReopenProject >/dev/null
cat >"$home2/.autopilot/projects/ReopenProject/TODO.md" <<'EOF'
# ReopenProject TODO

- [ ] reopen task
EOF
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="first pass" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start ReopenProject >/dev/null
cat >"$home2/.autopilot/projects/ReopenProject/TODO.md" <<'EOF'
# ReopenProject TODO

- [ ] reopen task
EOF
sleep 1
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="second pass" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start ReopenProject >/dev/null
assert_file_contains "$home2/.autopilot/projects/ReopenProject/runtime/state/tasks/task-0001.json" "\"attempt_count\": 2"
assert_file_contains "$home2/.autopilot/projects/ReopenProject/runtime/events/events.jsonl" "\"from\": \"done\", \"to\": \"todo\""

# Case 10:
# TODO update conflicts should leave task state done while recording the conflict.
echo "[test] keeps task state done when TODO update conflicts after success"
conflict_repo="$TMP_DIR/conflict-repo"
mkdir -p "$conflict_repo"
HOME="$home2" "$AP_BIN" new ConflictProject >/dev/null
HOME="$home2" "$AP_BIN" add "$conflict_repo" -n main -p ConflictProject >/dev/null
cat >"$home2/.autopilot/projects/ConflictProject/TODO.md" <<'EOF'
# ConflictProject TODO

- [ ] conflict task
EOF
stdout10="$TMP_DIR/start_stdout10.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="conflict summary" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  FAKE_AGENT_MUTATE_TODO_FILE="$home2/.autopilot/projects/ConflictProject/TODO.md" \
  FAKE_AGENT_MUTATE_TODO_FROM="- [ ] conflict task" \
  FAKE_AGENT_MUTATE_TODO_TO="- [ ] conflict task changed" \
  "$AP_BIN" start ConflictProject >"$stdout10"
conflict_run_dir="$(latest_run_dir "$home2/.autopilot/projects/ConflictProject")"
assert_file_contains "$stdout10" "completed task: conflict task"
assert_file_contains "$home2/.autopilot/projects/ConflictProject/TODO.md" "- [ ] conflict task changed"
assert_file_contains "$conflict_run_dir/result.json" "\"todo_update_applied\": false"
assert_file_contains "$home2/.autopilot/projects/ConflictProject/runtime/state/tasks/task-0001.json" "\"status\": \"done\""
assert_file_contains "$home2/.autopilot/projects/ConflictProject/runtime/state/tasks/task-0001.json" "\"last_error\": null"
assert_file_contains "$home2/.autopilot/projects/ConflictProject/runtime/events/events.jsonl" "\"type\": \"todo.sync_conflict\""

# Case 11:
# Duplicate unfinished TODO titles are rejected in Phase 2.
echo "[test] fails on duplicate unfinished TODO titles"
duplicate_repo="$TMP_DIR/duplicate-repo"
mkdir -p "$duplicate_repo"
HOME="$home2" "$AP_BIN" new DuplicateProject >/dev/null
HOME="$home2" "$AP_BIN" add "$duplicate_repo" -n main -p DuplicateProject >/dev/null
cat >"$home2/.autopilot/projects/DuplicateProject/TODO.md" <<'EOF'
# DuplicateProject TODO

- [ ] same title
- [ ] same title
EOF
stderr11="$TMP_DIR/start_stderr11.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start DuplicateProject > /dev/null 2>"$stderr11"
status11=$?
set -e
if [[ "$status11" -eq 0 ]]; then
  echo "assert failed: expected duplicate unfinished titles to fail" >&2
  exit 1
fi
assert_file_contains "$stderr11" "ap start failed: duplicate TODO task titles are not supported in Phase 2"
assert_file_contains "$home2/.autopilot/projects/DuplicateProject/runtime/events/events.jsonl" "\"type\": \"todo.sync_conflict\""

# Case 12:
# Pre-launch failures should roll back persisted in_progress state, and stale in_progress should be recovered.
echo "[test] rolls back pre-launch failures and recovers stale in_progress tasks"
cat >"$home2/.autopilot/config.toml" <<'EOF'
[start]
agent = "claude"
EOF
rollback_repo="$TMP_DIR/rollback-repo"
mkdir -p "$rollback_repo"
HOME="$home2" "$AP_BIN" new RollbackProject >/dev/null
HOME="$home2" "$AP_BIN" add "$rollback_repo" -n main -p RollbackProject >/dev/null
cat >"$home2/.autopilot/projects/RollbackProject/TODO.md" <<'EOF'
# RollbackProject TODO

- [ ] recoverable task
EOF
mkdir -p "$home2/.autopilot/projects/RollbackProject/runtime/state/tasks"
cat >"$home2/.autopilot/projects/RollbackProject/runtime/state/tasks/task-0001.json" <<'EOF'
{
  "id": "task-0001",
  "title": "recoverable task",
  "status": "todo",
  "source_file": "TODO.md",
  "source_line": 3,
  "source_text": "- [ ] recoverable task",
  "present_in_todo": true,
  "attempt_count": 0,
  "latest_run_id": null,
  "last_error": null,
  "created_at": "2026-03-15T00:00:00+09:00",
  "updated_at": "2026-03-15T00:00:00+09:00"
}
EOF
stderr12="$TMP_DIR/start_stderr12.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" AUTOPILOT_TEST_FAIL_EVENT_TYPE="task.selected" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start RollbackProject > /dev/null 2>"$stderr12"
status12=$?
set -e
if [[ "$status12" -eq 0 ]]; then
  echo "assert failed: expected pre-launch event failure to fail RollbackProject" >&2
  exit 1
fi
assert_file_contains "$stderr12" "ap start failed: failed to append event log"
assert_file_contains "$home2/.autopilot/projects/RollbackProject/runtime/state/tasks/task-0001.json" "\"status\": \"todo\""
assert_file_contains "$home2/.autopilot/projects/RollbackProject/runtime/state/tasks/task-0001.json" "\"attempt_count\": 0"
assert_file_contains "$home2/.autopilot/projects/RollbackProject/runtime/state/tasks/task-0001.json" "\"latest_run_id\": null"
assert_file_contains "$home2/.autopilot/projects/RollbackProject/runtime/state/tasks/task-0001.json" "\"last_error\": null"
cat >"$home2/.autopilot/projects/RollbackProject/runtime/state/tasks/task-0001.json" <<'EOF'
{
  "id": "task-0001",
  "title": "recoverable task",
  "status": "in_progress",
  "source_file": "TODO.md",
  "source_line": 3,
  "source_text": "- [ ] recoverable task",
  "present_in_todo": true,
  "attempt_count": 1,
  "latest_run_id": "run-stale-L3",
  "last_error": null,
  "created_at": "2026-03-15T00:00:00+09:00",
  "updated_at": "2026-03-15T00:01:00+09:00"
}
EOF
stdout12="$TMP_DIR/start_stdout12.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="recovered run" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start RollbackProject >"$stdout12"
assert_file_contains "$stdout12" "completed task: recoverable task"
assert_file_contains "$home2/.autopilot/projects/RollbackProject/TODO.md" "- [x] recoverable task"
assert_file_contains "$home2/.autopilot/projects/RollbackProject/runtime/state/tasks/task-0001.json" "\"status\": \"done\""
assert_file_contains "$home2/.autopilot/projects/RollbackProject/runtime/state/tasks/task-0001.json" "\"attempt_count\": 2"
assert_file_contains "$home2/.autopilot/projects/RollbackProject/runtime/events/events.jsonl" "\"from\": \"in_progress\", \"to\": \"failed\""

# Case 12b:
# Post-run event failures should not leave the task stuck in_progress.
echo "[test] does not leave completed runs stuck in_progress when post-run replay fails"
postrun_repo="$TMP_DIR/postrun-repo"
mkdir -p "$postrun_repo"
HOME="$home2" "$AP_BIN" new PostRunFailureProject >/dev/null
HOME="$home2" "$AP_BIN" add "$postrun_repo" -n main -p PostRunFailureProject >/dev/null
cat >"$home2/.autopilot/projects/PostRunFailureProject/TODO.md" <<'EOF'
# PostRunFailureProject TODO

- [ ] persisted task
EOF
postrun_events="$home2/.autopilot/projects/PostRunFailureProject/runtime/events/events.jsonl"
stderr12b="$TMP_DIR/start_stderr12b.txt"
set +e
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" AUTOPILOT_START_DISABLE_TMUX=1 \
  AUTOPILOT_TEST_FAIL_EVENT_TYPE="run.stdout" FAKE_AGENT_STDOUT="persisted task complete" \
  FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start PostRunFailureProject > /dev/null 2>"$stderr12b"
status12b=$?
set -e
if [[ "$status12b" -eq 0 ]]; then
  echo "assert failed: expected post-run event failure to fail PostRunFailureProject" >&2
  exit 1
fi
assert_file_contains "$stderr12b" "ap start failed: failed to append event log"
assert_file_contains "$home2/.autopilot/projects/PostRunFailureProject/TODO.md" "- [x] persisted task"
assert_file_contains "$home2/.autopilot/projects/PostRunFailureProject/runtime/state/tasks/task-0001.json" "\"status\": \"done\""
assert_file_not_contains "$home2/.autopilot/projects/PostRunFailureProject/runtime/state/tasks/task-0001.json" "\"status\": \"in_progress\""
assert_file_contains "$home2/.autopilot/projects/PostRunFailureProject/runtime/state/project.json" "\"done\": 1"
assert_file_not_contains "$home2/.autopilot/projects/PostRunFailureProject/runtime/state/project.json" "\"in_progress\": 1"
assert_file_not_contains "$postrun_events" "\"type\": \"run.stdout\""

# Case 12c:
# Marker strings mentioned in prose should not trigger blocked classification.
echo "[test] ignores blocker markers that are not at line start"
quoted_repo="$TMP_DIR/quoted-marker-repo"
mkdir -p "$quoted_repo"
HOME="$home2" "$AP_BIN" new QuotedMarkerProject >/dev/null
HOME="$home2" "$AP_BIN" add "$quoted_repo" -n main -p QuotedMarkerProject >/dev/null
cat >"$home2/.autopilot/projects/QuotedMarkerProject/TODO.md" <<'EOF'
# QuotedMarkerProject TODO

- [ ] quoted marker task
EOF
stdout12c="$TMP_DIR/start_stdout12c.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" AUTOPILOT_START_DISABLE_TMUX=1 \
  FAKE_AGENT_STDOUT="The spec literal AUTOPILOT_APPROVAL_REQUIRED: is documented here" \
  FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start QuotedMarkerProject >"$stdout12c"
assert_file_contains "$stdout12c" "completed task: quoted marker task"
assert_file_contains "$home2/.autopilot/projects/QuotedMarkerProject/TODO.md" "- [x] quoted marker task"
assert_file_contains "$home2/.autopilot/projects/QuotedMarkerProject/runtime/state/tasks/task-0001.json" "\"status\": \"done\""
assert_file_not_contains "$home2/.autopilot/projects/QuotedMarkerProject/runtime/events/events.jsonl" "\"type\": \"task.blocked\""

# Case 13:
# Multiple historical tasks with the same title should force a new task id instead of reusing history.
echo "[test] creates a new task id when multiple historical tasks share a title"
historical_repo="$TMP_DIR/historical-title-repo"
mkdir -p "$historical_repo"
HOME="$home2" "$AP_BIN" new HistoricalTitleProject >/dev/null
HOME="$home2" "$AP_BIN" add "$historical_repo" -n main -p HistoricalTitleProject >/dev/null
cat >"$home2/.autopilot/projects/HistoricalTitleProject/TODO.md" <<'EOF'
# HistoricalTitleProject TODO

- [ ] repeated task
EOF
mkdir -p "$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks"
cat >"$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0001.json" <<'EOF'
{
  "id": "task-0001",
  "title": "repeated task",
  "status": "done",
  "source_file": "TODO.md",
  "source_line": 7,
  "source_text": "- [x] repeated task",
  "present_in_todo": false,
  "attempt_count": 2,
  "latest_run_id": "run-old-1",
  "last_error": null,
  "created_at": "2026-03-15T00:00:00+09:00",
  "updated_at": "2026-03-15T00:05:00+09:00"
}
EOF
cat >"$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0002.json" <<'EOF'
{
  "id": "task-0002",
  "title": "repeated task",
  "status": "failed",
  "source_file": "TODO.md",
  "source_line": 9,
  "source_text": "- [ ] repeated task",
  "present_in_todo": false,
  "attempt_count": 4,
  "latest_run_id": "run-old-2",
  "last_error": "old failure",
  "created_at": "2026-03-15T00:00:00+09:00",
  "updated_at": "2026-03-15T00:06:00+09:00"
}
EOF
stdout13="$TMP_DIR/start_stdout13.txt"
env -u TMUX HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="fresh task history" \
  FAKE_AGENT_NAME="claude" FAKE_TMUX_STATE_DIR="$tmux_state" \
  "$AP_BIN" start HistoricalTitleProject >"$stdout13"
assert_file_contains "$stdout13" "completed task: repeated task"
assert_exists "$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0003.json"
assert_file_contains "$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0003.json" "\"status\": \"done\""
assert_file_contains "$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0003.json" "\"attempt_count\": 1"
assert_file_contains "$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0003.json" "\"source_line\": 3"
assert_file_contains "$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0001.json" "\"attempt_count\": 2"
assert_file_contains "$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0002.json" "\"attempt_count\": 4"
assert_file_contains "$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0001.json" "\"present_in_todo\": false"
assert_file_contains "$home2/.autopilot/projects/HistoricalTitleProject/runtime/state/tasks/task-0002.json" "\"present_in_todo\": false"

echo "all tests passed"
