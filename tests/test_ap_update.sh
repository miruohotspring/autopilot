#!/usr/bin/env bash
set -euo pipefail

# Integration test for `ap update`.
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

assert_same_file() {
  local actual="$1"
  local expected="$2"
  if ! cmp -s "$actual" "$expected"; then
    echo "assert failed: files differ" >&2
    echo "actual  : $actual" >&2
    echo "expected: $expected" >&2
    exit 1
  fi
}

assert_symlink_target() {
  local path="$1"
  local expected="$2"
  if [[ ! -L "$path" ]]; then
    echo "assert failed: expected symlink: $path" >&2
    exit 1
  fi
  local actual
  actual="$(readlink "$path")"
  if [[ "$actual" != "$expected" ]]; then
    echo "assert failed: symlink target mismatch for $path" >&2
    echo "expected: $expected" >&2
    echo "actual:   $actual" >&2
    exit 1
  fi
}

# Case 1:
# Without ~/.autopilot, command should fail with guidance.
echo "[test] fails when ~/.autopilot does not exist"
home1="$TMP_DIR/home1"
mkdir -p "$home1"
stderr1="$TMP_DIR/stderr1.txt"
set +e
(cd "$ROOT_DIR" && HOME="$home1" "$AP_BIN" update 2>"$stderr1")
status1=$?
set -e
if [[ "$status1" -eq 0 ]]; then
  echo "assert failed: expected ap update to fail when ~/.autopilot is missing" >&2
  exit 1
fi
assert_file_contains "$stderr1" "Please run ap init first"

# Case 2:
# Running from repo root should sync managed templates into ~/.autopilot.
echo "[test] syncs managed files from templates/autopilot"
home2="$TMP_DIR/home2"
mkdir -p "$home2/.autopilot" "$home2/.codex" "$home2/.claude"
echo "old content" >"$home2/.autopilot/CLAUDE.md"
stdout2="$TMP_DIR/stdout2.txt"
stderr2="$TMP_DIR/stderr2.txt"
set +e
(cd "$ROOT_DIR" && HOME="$home2" "$AP_BIN" update >"$stdout2" 2>"$stderr2")
status2=$?
set -e
if [[ "$status2" -ne 0 ]]; then
  echo "assert failed: expected ap update to succeed from repository root" >&2
  cat "$stderr2" >&2
  exit 1
fi

for rel in \
  "CLAUDE.md" \
  "config.toml" \
  "skills/ap-self-recognition/SKILL.md" \
  "skills/ap-self-recognition/agents/openai.yaml" \
  "skills/ap-briefing/SKILL.md" \
  "skills/ap-briefing/agents/openai.yaml"; do
  assert_exists "$home2/.autopilot/$rel"
done
assert_file_contains "$stdout2" "updated: \"$home2/.autopilot/CLAUDE.md\""
assert_same_file "$home2/.autopilot/CLAUDE.md" "$ROOT_DIR/templates/autopilot/CLAUDE.md"
assert_same_file "$home2/.autopilot/config.toml" "$ROOT_DIR/templates/autopilot/config.toml"
assert_same_file \
  "$home2/.autopilot/skills/ap-self-recognition/SKILL.md" \
  "$ROOT_DIR/templates/autopilot/skills/ap-self-recognition/SKILL.md"
assert_same_file \
  "$home2/.autopilot/skills/ap-self-recognition/agents/openai.yaml" \
  "$ROOT_DIR/templates/autopilot/skills/ap-self-recognition/agents/openai.yaml"
assert_same_file \
  "$home2/.autopilot/skills/ap-briefing/SKILL.md" \
  "$ROOT_DIR/templates/autopilot/skills/ap-briefing/SKILL.md"
assert_same_file \
  "$home2/.autopilot/skills/ap-briefing/agents/openai.yaml" \
  "$ROOT_DIR/templates/autopilot/skills/ap-briefing/agents/openai.yaml"
assert_file_contains "$stdout2" "updated: \"$home2/.autopilot/skills/ap-self-recognition/SKILL.md\""
assert_file_contains "$stdout2" "updated: \"$home2/.codex/skills/ap-self-recognition\""
assert_file_contains "$stdout2" "updated: \"$home2/.claude/skills/ap-briefing\""
assert_symlink_target "$home2/.codex/skills/ap-self-recognition" "../../.autopilot/skills/ap-self-recognition"
assert_symlink_target "$home2/.codex/skills/ap-briefing" "../../.autopilot/skills/ap-briefing"
assert_symlink_target "$home2/.claude/skills/ap-self-recognition" "../../.autopilot/skills/ap-self-recognition"
assert_symlink_target "$home2/.claude/skills/ap-briefing" "../../.autopilot/skills/ap-briefing"

# Case 3:
# Existing managed skill directories should be replaced by symlinks without touching others.
echo "[test] replaces only managed home skill directories with symlinks"
home3="$TMP_DIR/home3"
mkdir -p \
  "$home3/.autopilot" \
  "$home3/.codex/skills/ap-self-recognition" \
  "$home3/.codex/skills/.system" \
  "$home3/.claude/skills/ap-briefing" \
  "$home3/.claude/skills/.system"
echo "legacy" >"$home3/.codex/skills/ap-self-recognition/SKILL.md"
echo "keep" >"$home3/.codex/skills/.system/SKILL.md"
echo "legacy" >"$home3/.claude/skills/ap-briefing/SKILL.md"
echo "keep" >"$home3/.claude/skills/.system/SKILL.md"
stdout3="$TMP_DIR/stdout3.txt"
stderr3="$TMP_DIR/stderr3.txt"
set +e
(cd "$ROOT_DIR" && HOME="$home3" "$AP_BIN" update >"$stdout3" 2>"$stderr3")
status3=$?
set -e
if [[ "$status3" -ne 0 ]]; then
  echo "assert failed: expected ap update to migrate managed home skills directories" >&2
  cat "$stderr3" >&2
  exit 1
fi
assert_exists "$home3/.autopilot/skills/ap-self-recognition/SKILL.md"
assert_symlink_target "$home3/.codex/skills/ap-self-recognition" "../../.autopilot/skills/ap-self-recognition"
assert_symlink_target "$home3/.codex/skills/ap-briefing" "../../.autopilot/skills/ap-briefing"
assert_symlink_target "$home3/.claude/skills/ap-self-recognition" "../../.autopilot/skills/ap-self-recognition"
assert_symlink_target "$home3/.claude/skills/ap-briefing" "../../.autopilot/skills/ap-briefing"
assert_exists "$home3/.codex/skills/.system/SKILL.md"
assert_exists "$home3/.claude/skills/.system/SKILL.md"

# Case 4:
# Self-referential managed skill symlinks under ~/.autopilot should be repaired.
echo "[test] repairs self-referential managed skill symlinks"
home4="$TMP_DIR/home4"
mkdir -p "$home4/.autopilot/skills" "$home4/.codex" "$home4/.claude"
ln -s ../../.autopilot/skills/ap-self-recognition \
  "$home4/.autopilot/skills/ap-self-recognition"
stdout4="$TMP_DIR/stdout4.txt"
stderr4="$TMP_DIR/stderr4.txt"
set +e
(cd "$ROOT_DIR" && HOME="$home4" "$AP_BIN" update >"$stdout4" 2>"$stderr4")
status4=$?
set -e
if [[ "$status4" -ne 0 ]]; then
  echo "assert failed: expected ap update to repair self-referential managed skill symlink" >&2
  cat "$stderr4" >&2
  exit 1
fi
assert_exists "$home4/.autopilot/skills/ap-self-recognition/SKILL.md"
assert_same_file \
  "$home4/.autopilot/skills/ap-self-recognition/SKILL.md" \
  "$ROOT_DIR/templates/autopilot/skills/ap-self-recognition/SKILL.md"
assert_symlink_target "$home4/.codex/skills/ap-self-recognition" "../../.autopilot/skills/ap-self-recognition"

# Case 5:
# Outside repo root, command should fail to locate template directory.
echo "[test] fails outside repository root"
home5="$TMP_DIR/home5"
mkdir -p "$home5/.autopilot"
stderr5="$TMP_DIR/stderr5.txt"
set +e
(cd "$TMP_DIR" && HOME="$home5" "$AP_BIN" update 2>"$stderr5")
status5=$?
set -e
if [[ "$status5" -eq 0 ]]; then
  echo "assert failed: expected ap update to fail outside repository root" >&2
  exit 1
fi
assert_file_contains "$stderr5" "run from autopilot repository root"

echo "all tests passed"
