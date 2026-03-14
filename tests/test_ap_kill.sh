#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap kill`.
# Uses a fake tmux binary to validate commands without requiring real tmux.
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

FAKE_BIN="$TMP_DIR/fake-bin"
FAKE_TMUX="$FAKE_BIN/tmux"
mkdir -p "$FAKE_BIN"

cat >"$FAKE_TMUX" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

STATE_DIR="${FAKE_TMUX_STATE_DIR:?}"
LOG_FILE="$STATE_DIR/tmux.log"
SESSION_FILE="$STATE_DIR/autopilot.session"

cmd="${1:-}"
shift || true

printf "%s %s\n" "$cmd" "$*" >>"$LOG_FILE"

case "$cmd" in
  has-session)
    if [[ -f "$SESSION_FILE" ]]; then
      exit 0
    fi
    exit 1
    ;;
  kill-session)
    if [[ "${FAKE_TMUX_FAIL_KILL:-0}" == "1" ]]; then
      exit 1
    fi
    rm -f "$SESSION_FILE"
    exit 0
    ;;
  *)
    echo "unexpected tmux command: $cmd" >&2
    exit 99
    ;;
esac
EOF
chmod +x "$FAKE_TMUX"

# Case 1:
# If session does not exist, ap kill should succeed and not call kill-session.
echo "[test] succeeds when session does not exist"
state1="$TMP_DIR/state1"
home1="$TMP_DIR/home1"
mkdir -p "$state1" "$home1/.autopilot"
stdout1="$TMP_DIR/kill_stdout1.txt"
stderr1="$TMP_DIR/kill_stderr1.txt"
set +e
HOME="$home1" \
  PATH="$FAKE_BIN:$PATH" \
  FAKE_TMUX_STATE_DIR="$state1" \
  "$AP_BIN" kill >"$stdout1" 2>"$stderr1"
status1=$?
set -e
if [[ "$status1" -ne 0 ]]; then
  echo "assert failed: expected ap kill to succeed when session does not exist" >&2
  exit 1
fi
assert_file_contains "$state1/tmux.log" "has-session -t autopilot"
assert_file_not_contains "$state1/tmux.log" "kill-session -t autopilot"
assert_file_contains "$stdout1" "no tmux session: autopilot"

# Case 2:
# If session exists, ap kill should terminate it.
echo "[test] kills existing autopilot session"
state2="$TMP_DIR/state2"
home2="$TMP_DIR/home2"
mkdir -p "$state2" "$home2/.autopilot"
touch "$state2/autopilot.session"
stdout2="$TMP_DIR/kill_stdout2.txt"
stderr2="$TMP_DIR/kill_stderr2.txt"
set +e
HOME="$home2" \
  PATH="$FAKE_BIN:$PATH" \
  FAKE_TMUX_STATE_DIR="$state2" \
  "$AP_BIN" kill >"$stdout2" 2>"$stderr2"
status2=$?
set -e
if [[ "$status2" -ne 0 ]]; then
  echo "assert failed: expected ap kill to succeed when session exists" >&2
  exit 1
fi
assert_file_contains "$state2/tmux.log" "has-session -t autopilot"
assert_file_contains "$state2/tmux.log" "kill-session -t autopilot"
assert_file_contains "$stdout2" "killed tmux session: autopilot"

# Case 3:
# If kill-session fails, ap kill should return non-zero.
echo "[test] returns non-zero when kill-session fails"
state3="$TMP_DIR/state3"
home3="$TMP_DIR/home3"
mkdir -p "$state3" "$home3/.autopilot"
touch "$state3/autopilot.session"
stdout3="$TMP_DIR/kill_stdout3.txt"
stderr3="$TMP_DIR/kill_stderr3.txt"
set +e
HOME="$home3" \
  PATH="$FAKE_BIN:$PATH" \
  FAKE_TMUX_STATE_DIR="$state3" \
  FAKE_TMUX_FAIL_KILL=1 \
  "$AP_BIN" kill >"$stdout3" 2>"$stderr3"
status3=$?
set -e
if [[ "$status3" -eq 0 ]]; then
  echo "assert failed: expected ap kill to fail when tmux kill-session fails" >&2
  exit 1
fi
assert_file_contains "$state3/tmux.log" "kill-session -t autopilot"
assert_file_contains "$stderr3" "ap kill failed: failed to kill tmux session"

# Case 4:
# Invalid invocation should return non-zero.
echo "[test] rejects unexpected arguments"
set +e
"$AP_BIN" kill extra >/dev/null 2>&1
status4=$?
set -e
if [[ "$status4" -eq 0 ]]; then
  echo "assert failed: expected invalid kill arguments to fail" >&2
  exit 1
fi

echo "all tests passed"
