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
printf '%s\n' "${FAKE_AGENT_NAME:-agent}:${FAKE_AGENT_STDOUT:-fake agent completed}"
printf '%s\n' "${FAKE_AGENT_NAME:-agent}:${FAKE_AGENT_STDERR:-}" >&2
exit "${FAKE_AGENT_EXIT_CODE:-0}"
EOF
  chmod +x "$dir/$name"
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
printf '2\n' | env HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="beta summary" \
  FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start >"$stdout2"
assert_file_contains "$stdout2" "Select project to start:"
assert_file_contains "$stdout2" "Enter number to start:"
assert_file_contains "$stdout2" "completed task: beta task"
assert_file_contains "$home2/.autopilot/projects/BetaProject/TODO.md" "- [x] beta task"

# Reset BetaProject TODO because project ordering is lexical and project output text checks are simpler below.
cat >"$home2/.autopilot/projects/BetaProject/TODO.md" <<'EOF'
# BetaProject TODO

- [ ] beta task
EOF

# Case 3:
# Success should create runtime artifacts and mark the first open task done.
echo "[test] creates runtime artifacts and marks first task done on success"
stdout3="$TMP_DIR/start_stdout3.txt"
HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="implemented first task" \
  FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start AlphaProject >"$stdout3"
assert_file_contains "$stdout3" "completed task: first task"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/TODO.md" "- [x] first task"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/TODO.md" "- [ ] second task"
alpha_run_dir="$(latest_run_dir "$home2/.autopilot/projects/AlphaProject")"
assert_exists "$alpha_run_dir/meta.json"
assert_exists "$alpha_run_dir/prompt.txt"
assert_exists "$alpha_run_dir/stdout.log"
assert_exists "$alpha_run_dir/stderr.log"
assert_exists "$alpha_run_dir/result.json"
assert_file_contains "$alpha_run_dir/result.json" "\"status\": \"succeeded\""
assert_file_contains "$alpha_run_dir/result.json" "\"todo_update_applied\": true"
assert_file_contains "$alpha_run_dir/result.json" "implemented first task"
assert_file_contains "$home2/.autopilot/projects/AlphaProject/runtime/last_run.json" "\"status\": \"succeeded\""

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
HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_EXIT_CODE=9 FAKE_AGENT_STDOUT="partial log" \
  FAKE_AGENT_NAME="claude" \
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
assert_exists "$failure_run_dir/result.json"
assert_file_contains "$failure_run_dir/result.json" "\"status\": \"failed\""
assert_file_contains "$failure_run_dir/result.json" "\"todo_update_applied\": false"
assert_file_contains "$failure_run_dir/stdout.log" "partial log"

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
HOME="$home2" PATH="$fake_bin:$PATH" "$AP_BIN" start MultiProject 2>"$stderr5" >/dev/null
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
HOME="$home2" PATH="$fake_bin:$PATH" "$AP_BIN" start EmptyProject 2>"$stderr6" >/dev/null
status6=$?
set -e
if [[ "$status6" -eq 0 ]]; then
  echo "assert failed: expected no-task project to fail" >&2
  exit 1
fi
assert_file_contains "$stderr6" "ap start failed: no runnable task found in TODO.md"

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
HOME="$home2" PATH="$fake_bin:$PATH" FAKE_AGENT_STDOUT="configured agent run" \
  FAKE_AGENT_NAME="codex" "$AP_BIN" start ConfigProject >"$stdout7"
config_run_dir="$(latest_run_dir "$home2/.autopilot/projects/ConfigProject")"
assert_file_contains "$stdout7" "completed task: config task"
assert_file_contains "$config_run_dir/meta.json" "\"agent\": \"codex\""
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
HOME="$home2" PATH="$fake_claude_only" FAKE_AGENT_NAME="claude" \
  "$AP_BIN" start MissingAgentProject 2>"$stderr8" >/dev/null
status8=$?
set -e
if [[ "$status8" -eq 0 ]]; then
  echo "assert failed: expected missing configured agent to fail" >&2
  exit 1
fi
assert_file_contains "$stderr8" "ap start failed: configured agent CLI not found: codex"

echo "all tests passed"
