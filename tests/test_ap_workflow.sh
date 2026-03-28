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

home="$TMP_DIR/home"
repo="$TMP_DIR/repo"
mkdir -p "$home/.autopilot" "$repo"

HOME="$home" "$AP_BIN" new Demo demo >/dev/null
HOME="$home" "$AP_BIN" add "$repo" -n main -p Demo >/dev/null
HOME="$home" "$AP_BIN" task add "First task" -p Demo >/dev/null
HOME="$home" "$AP_BIN" task add "Second task" -p Demo >/dev/null

cat >"$home/.autopilot/config.toml" <<'EOF'
[start]
agent = "human"
reviewer_agent = "human"
EOF

echo "[test] task sync and task next expose the runnable task"
sync_stdout="$TMP_DIR/task_sync_stdout.txt"
HOME="$home" "$AP_BIN" task sync Demo >"$sync_stdout"
assert_file_contains "$sync_stdout" "synced task state: Demo"
next_stdout="$TMP_DIR/task_next_stdout.txt"
HOME="$home" "$AP_BIN" task next Demo >"$next_stdout"
assert_file_contains "$next_stdout" $'demo-0001\tFirst task'

echo "[test] run start publishes a manual human coder run"
run_start_stdout="$TMP_DIR/run_start_stdout.txt"
HOME="$home" "$AP_BIN" run start Demo >"$run_start_stdout"
assert_file_contains "$run_start_stdout" "actor=human"
run_id="$(awk -F= '/^run_id=/{print $2}' "$run_start_stdout")"
task_state_1="$home/.autopilot/projects/Demo/runtime/state/tasks/demo-0001.json"
run_meta_1="$home/.autopilot/projects/Demo/runtime/runs/$run_id/meta.json"
assert_file_contains "$run_meta_1" '"status": "published"'
assert_file_contains "$task_state_1" '"status": "in_progress"'

echo "[test] recover does not consume published manual runs"
recover_stdout_1="$TMP_DIR/recover_stdout_1.txt"
HOME="$home" "$AP_BIN" recover Demo >"$recover_stdout_1"
assert_file_contains "$recover_stdout_1" "recovered tasks: 0"
assert_file_contains "$task_state_1" '"status": "in_progress"'

echo "[test] run finish can park the task in review_pending without touching TODO"
run_finish_stdout="$TMP_DIR/run_finish_stdout.txt"
HOME="$home" "$AP_BIN" run finish "$run_id" --status done --review --summary "manual done" \
  >"$run_finish_stdout"
assert_file_contains "$run_finish_stdout" "task_status=review_pending"
assert_file_contains "$task_state_1" '"status": "review_pending"'
todo_file="$home/.autopilot/projects/Demo/TODO.md"
assert_file_not_contains "$todo_file" "- [x] [demo-0001] First task"

echo "[test] recover keeps review_pending tasks that are waiting for explicit review publication"
recover_stdout_2="$TMP_DIR/recover_stdout_2.txt"
HOME="$home" "$AP_BIN" recover Demo >"$recover_stdout_2"
assert_file_contains "$recover_stdout_2" "recovered tasks: 0"
assert_file_contains "$task_state_1" '"status": "review_pending"'

echo "[test] review start publishes a manual human reviewer run"
review_start_stdout="$TMP_DIR/review_start_stdout.txt"
HOME="$home" "$AP_BIN" review start "$run_id" >"$review_start_stdout"
assert_file_contains "$review_start_stdout" "actor=human"
review_id="$(awk -F= '/^run_id=/{print $2}' "$review_start_stdout")"
review_meta="$home/.autopilot/projects/Demo/runtime/runs/$review_id/meta.json"
assert_file_contains "$review_meta" '"status": "published"'

echo "[test] review submit approve completes the task and updates TODO"
review_submit_stdout="$TMP_DIR/review_submit_stdout.txt"
HOME="$home" "$AP_BIN" review submit "$review_id" --verdict approve --summary "looks good" \
  >"$review_submit_stdout"
assert_file_contains "$review_submit_stdout" "task_status=done"
assert_file_contains "$task_state_1" '"status": "done"'
assert_file_contains "$todo_file" "- [x] [demo-0001] First task"

echo "[test] review submit creates cycle exceeded alert for manual workflow"
cycle_repo="$TMP_DIR/cycle-repo"
mkdir -p "$cycle_repo"
HOME="$home" "$AP_BIN" new CycleDemo cycledemo >/dev/null
HOME="$home" "$AP_BIN" add "$cycle_repo" -n main -p CycleDemo >/dev/null
HOME="$home" "$AP_BIN" task add "Cycle task" -p CycleDemo >/dev/null
cycle_run_start_stdout="$TMP_DIR/cycle_run_start_stdout.txt"
HOME="$home" "$AP_BIN" run start CycleDemo >"$cycle_run_start_stdout"
cycle_run_id="$(awk -F= '/^run_id=/{print $2}' "$cycle_run_start_stdout")"
cycle_task_state="$home/.autopilot/projects/CycleDemo/runtime/state/tasks/cycledemo-0001.json"
perl -0pi -e 's/"max_review_cycles": null/"max_review_cycles": 0/' "$cycle_task_state"
HOME="$home" "$AP_BIN" run finish "$cycle_run_id" --status done --review --summary "manual done" \
  >/dev/null
cycle_review_start_stdout="$TMP_DIR/cycle_review_start_stdout.txt"
HOME="$home" "$AP_BIN" review start "$cycle_run_id" >"$cycle_review_start_stdout"
cycle_review_id="$(awk -F= '/^run_id=/{print $2}' "$cycle_review_start_stdout")"
cycle_review_submit_stdout="$TMP_DIR/cycle_review_submit_stdout.txt"
HOME="$home" "$AP_BIN" review submit "$cycle_review_id" --verdict rework \
  --issue "needs redesign" --suggestion "rethink approach" >"$cycle_review_submit_stdout"
assert_file_contains "$cycle_review_submit_stdout" "task_status=blocked"
assert_file_contains "$cycle_task_state" '"status": "blocked"'
assert_file_contains "$cycle_task_state" '"review_result": "rework"'
assert_file_contains "$cycle_task_state" '"blocker_category": "review_cycle_limit"'
cycle_events="$home/.autopilot/projects/CycleDemo/runtime/events/events.jsonl"
assert_file_contains "$cycle_events" '"type": "task.review_cycle_exceeded"'
assert_file_contains "$cycle_events" '"review_cycle_count": 1'
assert_file_contains "$cycle_events" '"max_review_cycles": 0'
cycle_alert="$(find "$home/.autopilot/projects/CycleDemo/runtime/alerts" -type f | head -n 1)"
assert_file_contains "$cycle_alert" '"type": "review_cycle_exceeded"'

echo "[test] recover promotes interrupted reviewer run to last_run and creates alert"
recover_repo="$TMP_DIR/recover-repo"
mkdir -p "$recover_repo"
HOME="$home" "$AP_BIN" new RecoverDemo recoverdemo >/dev/null
HOME="$home" "$AP_BIN" add "$recover_repo" -n main -p RecoverDemo >/dev/null
HOME="$home" "$AP_BIN" task add "Recover task" -p RecoverDemo >/dev/null
recover_run_start_stdout="$TMP_DIR/recover_run_start_stdout.txt"
HOME="$home" "$AP_BIN" run start RecoverDemo >"$recover_run_start_stdout"
recover_run_id="$(awk -F= '/^run_id=/{print $2}' "$recover_run_start_stdout")"
HOME="$home" "$AP_BIN" run finish "$recover_run_id" --status done --review --summary "manual done" \
  >/dev/null
recover_review_start_stdout="$TMP_DIR/recover_review_start_stdout.txt"
HOME="$home" "$AP_BIN" review start "$recover_run_id" >"$recover_review_start_stdout"
recover_review_id="$(awk -F= '/^run_id=/{print $2}' "$recover_review_start_stdout")"
recover_review_meta="$home/.autopilot/projects/RecoverDemo/runtime/runs/$recover_review_id/meta.json"
perl -0pi -e 's/"status": "published"/"status": "running"/' "$recover_review_meta"
recover_stdout_3="$TMP_DIR/recover_stdout_3.txt"
HOME="$home" "$AP_BIN" recover RecoverDemo >"$recover_stdout_3"
assert_file_contains "$recover_stdout_3" "recovered tasks: 1"
recover_task_state="$home/.autopilot/projects/RecoverDemo/runtime/state/tasks/recoverdemo-0001.json"
recover_project_state="$home/.autopilot/projects/RecoverDemo/runtime/state/project.json"
recover_events="$home/.autopilot/projects/RecoverDemo/runtime/events/events.jsonl"
assert_file_contains "$recover_task_state" '"status": "blocked"'
assert_file_contains "$recover_task_state" "\"latest_run_id\": \"$recover_review_id\""
assert_file_contains "$recover_task_state" "\"reviewer_run_id\": \"$recover_review_id\""
assert_file_contains "$recover_task_state" '"last_run_exit_reason": "internal_error"'
assert_file_contains "$recover_project_state" "\"last_run_id\": \"$recover_review_id\""
assert_file_not_contains "$recover_project_state" '"last_run_at": null'
assert_file_contains "$recover_events" '"type": "run.interrupted"'
recover_alert="$(find "$home/.autopilot/projects/RecoverDemo/runtime/alerts" -type f | head -n 1)"
assert_file_contains "$recover_alert" '"type": "reviewer_error"'
assert_file_contains "$recover_alert" '"run_id": "'"$recover_review_id"'"'

echo "[test] task set-status updates task state directly"
set_status_stdout="$TMP_DIR/task_set_status_stdout.txt"
HOME="$home" "$AP_BIN" task set-status demo-0002 --status blocked -p Demo >"$set_status_stdout"
assert_file_contains "$set_status_stdout" "updated task status: demo-0002 -> blocked"
task_state_2="$home/.autopilot/projects/Demo/runtime/state/tasks/demo-0002.json"
assert_file_contains "$task_state_2" '"status": "blocked"'
