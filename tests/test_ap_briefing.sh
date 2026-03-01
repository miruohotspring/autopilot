#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap briefing`.
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
COLONEL_FILE="$STATE_DIR/autopilot.colonel"

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
  new-session)
    touch "$SESSION_FILE"
    exit 0
    ;;
  new-window)
    touch "$SESSION_FILE"
    touch "$COLONEL_FILE"
    exit 0
    ;;
  list-windows)
    if [[ ! -f "$SESSION_FILE" ]]; then
      exit 1
    fi
    echo "zsh"
    if [[ -f "$COLONEL_FILE" ]]; then
      echo "colonel"
    fi
    exit 0
    ;;
  switch-client|attach-session|send-keys)
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
# Outside tmux: should create session and attach to autopilot:colonel.
echo "[test] creates and attaches session outside tmux"
state1="$TMP_DIR/state1"
home1="$TMP_DIR/home1"
mkdir -p "$state1" "$home1/.autopilot"
stdout1="$TMP_DIR/briefing_stdout1.txt"
stderr1="$TMP_DIR/briefing_stderr1.txt"
set +e
env -u TMUX \
  HOME="$home1" \
  PATH="$FAKE_BIN:$PATH" \
  FAKE_TMUX_STATE_DIR="$state1" \
  "$AP_BIN" briefing >"$stdout1" 2>"$stderr1"
status1=$?
set -e
if [[ "$status1" -ne 0 ]]; then
  echo "assert failed: expected ap briefing to succeed outside tmux" >&2
  exit 1
fi
assert_file_contains "$state1/tmux.log" "has-session -t autopilot"
assert_file_contains "$state1/tmux.log" "new-session -d -s autopilot"
assert_file_contains "$state1/tmux.log" "list-windows -t autopilot -F #{window_name}"
assert_file_contains "$state1/tmux.log" "new-window -t autopilot -n colonel"
assert_file_contains "$state1/tmux.log" "claude --model sonnet --dangerously-skip-permissions"
assert_file_contains "$state1/tmux.log" "--append-system-prompt"
assert_file_contains "$state1/tmux.log" "日本語"
assert_file_contains "$state1/tmux.log" "\$self-recognition"
assert_file_contains "$state1/tmux.log" "\$briefing"
assert_file_contains "$state1/tmux.log" "attach-session -t autopilot:colonel"
assert_file_not_contains "$state1/tmux.log" "switch-client -t autopilot:colonel"

# Case 2:
# Inside tmux: should create session and switch client.
echo "[test] creates and switches session inside tmux"
state2="$TMP_DIR/state2"
home2="$TMP_DIR/home2"
mkdir -p "$state2" "$home2/.autopilot"
set +e
TMUX="/tmp/fake-tmux-client" \
  HOME="$home2" \
  PATH="$FAKE_BIN:$PATH" \
  FAKE_TMUX_STATE_DIR="$state2" \
  "$AP_BIN" briefing >/dev/null 2>&1
status2=$?
set -e
if [[ "$status2" -ne 0 ]]; then
  echo "assert failed: expected ap briefing to succeed inside tmux" >&2
  exit 1
fi
assert_file_contains "$state2/tmux.log" "new-window -t autopilot -n colonel"
assert_file_contains "$state2/tmux.log" "switch-client -t autopilot:colonel"
assert_file_not_contains "$state2/tmux.log" "attach-session -t autopilot:colonel"

# Case 3:
# Existing session with colonel should reuse and attach.
echo "[test] reuses existing autopilot session with colonel window"
state3="$TMP_DIR/state3"
home3="$TMP_DIR/home3"
mkdir -p "$state3" "$home3/.autopilot"
touch "$state3/autopilot.session"
touch "$state3/autopilot.colonel"
stdout3="$TMP_DIR/briefing_stdout3.txt"
stderr3="$TMP_DIR/briefing_stderr3.txt"
set +e
env -u TMUX \
  HOME="$home3" \
  PATH="$FAKE_BIN:$PATH" \
  FAKE_TMUX_STATE_DIR="$state3" \
  "$AP_BIN" briefing >"$stdout3" 2>"$stderr3"
status3=$?
set -e
if [[ "$status3" -ne 0 ]]; then
  echo "assert failed: expected ap briefing to reuse existing session" >&2
  exit 1
fi
assert_file_not_contains "$state3/tmux.log" "new-session -d -s autopilot"
assert_file_not_contains "$state3/tmux.log" "new-window -t autopilot -n colonel"
assert_file_contains "$state3/tmux.log" "list-windows -t autopilot -F #{window_name}"
assert_file_contains "$state3/tmux.log" "send-keys -t autopilot:colonel"
assert_file_contains "$state3/tmux.log" "\$self-recognition"
assert_file_contains "$state3/tmux.log" "attach-session -t autopilot:colonel"
assert_file_not_contains "$state3/tmux.log" "switch-client -t autopilot:colonel"

# Case 4:
# Existing session without colonel should add the window and attach.
echo "[test] adds colonel window to existing autopilot session"
state4="$TMP_DIR/state4"
home4="$TMP_DIR/home4"
mkdir -p "$state4" "$home4/.autopilot"
touch "$state4/autopilot.session"
stdout4="$TMP_DIR/briefing_stdout4.txt"
stderr4="$TMP_DIR/briefing_stderr4.txt"
set +e
env -u TMUX \
  HOME="$home4" \
  PATH="$FAKE_BIN:$PATH" \
  FAKE_TMUX_STATE_DIR="$state4" \
  "$AP_BIN" briefing >"$stdout4" 2>"$stderr4"
status4=$?
set -e
if [[ "$status4" -ne 0 ]]; then
  echo "assert failed: expected ap briefing to add missing colonel window" >&2
  exit 1
fi
assert_file_not_contains "$state4/tmux.log" "new-session -d -s autopilot"
assert_file_contains "$state4/tmux.log" "list-windows -t autopilot -F #{window_name}"
assert_file_contains "$state4/tmux.log" "new-window -t autopilot -n colonel"
assert_file_not_contains "$state4/tmux.log" "switch-client -t autopilot:colonel"
assert_file_contains "$state4/tmux.log" "attach-session -t autopilot:colonel"

echo "all tests passed"
