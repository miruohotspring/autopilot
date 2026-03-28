# autopilot

## Build

```bash
make
```

## Licenses

- Project license: [LICENSE](LICENSE)
- Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

## Test

```bash
make test
```

Current test inventory:

- [docs/TEST_INVENTORY.md](docs/TEST_INVENTORY.md)

## Install

```bash
make install
```

If needed:

```bash
sudo make install
```

## Initialization

```bash
ap init
```

It will create `$HOME/.autopilot` directory.
It also creates `$HOME/.autopilot/projects`.
It also creates `$HOME/.autopilot/config.toml`.
It also creates Claude Code project skills in:

```text
$HOME/.autopilot/skills/ap-self-recognition
$HOME/.autopilot/skills/ap-briefing
```

If `$HOME/.codex` exists, it also creates these symlinks:

```text
$HOME/.codex/skills/ap-self-recognition -> ../../.autopilot/skills/ap-self-recognition
$HOME/.codex/skills/ap-briefing -> ../../.autopilot/skills/ap-briefing
```

If `$HOME/.claude` exists, it also creates:

```text
$HOME/.claude/skills/ap-self-recognition -> ../../.autopilot/skills/ap-self-recognition
$HOME/.claude/skills/ap-briefing -> ../../.autopilot/skills/ap-briefing
```

Run this command from the autopilot repository root.

## Update managed files

```bash
ap update
```

`ap update` copies managed files from this repository's `templates/autopilot` into
`$HOME/.autopilot`.

Run this command from the autopilot repository root.

## Start agent configuration

`ap start` reads the preferred agent from:

```text
$HOME/.autopilot/config.toml
```

Current format:

```toml
[start]
agent = "claude"
reviewer_agent = "claude"
```

Supported values are `claude`, `codex`, and `human`.

- `human` is for the manual workflow commands:
  - `ap run start`
  - `ap run finish`
  - `ap review start`
  - `ap review submit`
- `ap start` still requires CLI-backed agents. If `start.agent` or `start.reviewer_agent` is `human`, `ap start` fails.
- If `config.toml` sets `start.agent`, `ap start` and `ap run start` use that actor by default.
- If `config.toml` sets `start.reviewer_agent`, `ap review start` uses that actor by default.
- If the configured CLI is not found in `PATH`, CLI-backed workflows fail.
- If `start.agent` is not set, `ap start` falls back to automatic detection.

## New project

```bash
ap new <project_name> <slug>
```

If either value is omitted, `ap` asks interactively:

```text
Enter your new project name:
Enter project slug:
```

`project_name` can include letters, numbers, and hyphens (`-`), and cannot start or end with a hyphen.
`slug` must match `^[a-z][a-z0-9-]*$`.

`ap new` adds project entries with this structure:

```yaml
<project_name>:
  slug: '<project_slug>'
  priority: 1
  paths: []
```

It also creates project directory:

```text
$HOME/.autopilot/projects/<project_name>
```

`ap new` also creates:

- `TODO.md`
- `dashboard.md` (captain -> colonel status handoff template)

## Delete project

```bash
ap delete <project_name>
```

If `<project_name>` is omitted, `ap` asks interactively:

```text
Select project to delete:
```

In an interactive terminal, you can select with `↑/↓` or `j/k`, and press Enter:

```text
? Select project to delete (Use arrow keys or j/k)
```

When stdin/stdout is not a TTY (e.g. piped input), `ap` falls back to number selection:

```text
  1) ProjectA
  2) ProjectB
Enter number to delete:
```

Before deletion, `ap` asks for confirmation:

```text
Delete project '<project_name>'? [y/n]:
```

## List projects

```bash
ap list
```

Outputs project names one per line. If no project exists, it prints:

```text
no projects
```

## Add path

```bash
ap add <path> [-n <name>] [-p <project_name>]
```

If `-p <project_name>` is omitted, `ap` asks you to select a project:

```text
Select project to add path:
```

The given `<path>` is normalized and stored as an absolute path.

`-n <name>` is optional. If omitted, `ap` asks:

```text
Enter path name [main]:
```

When you press Enter without input, `main` is used as the default.
If `main` already exists in that project, empty input is rejected and explicit input is required.

`ap add` stores `slug`, `name`, and `path` metadata in `projects.yaml`.

`name` and `path` are one-to-one in each project:

- same `path` with different `name` is rejected
- same `name` with different `path` is rejected

If the same `name/path` pair already exists in the target project, `ap add` does nothing (not an error).

`ap add` also creates a symlink:

```text
$HOME/.autopilot/projects/<project_name>/<name> -> <path>
```

## Remove path

```bash
ap rm -p <project_name>
```

If `-p <project_name>` is omitted, `ap` asks you to select a project.

`ap rm` then shows the path list in that project, asks which path to remove, and confirms with `y/n`.

## Add task

```bash
ap task add <title> [-p <project_name>]
```

If `-p <project_name>` is omitted, `ap` asks you to select a project.

`ap task add` allocates a stable task ID using the project's slug and appends a visible ID line to `TODO.md`:

```markdown
- [ ] [demo-0001] Add ap start command
```

It also creates the matching runtime task state file.

`ap task add` requires the target project to already have at least one managed path. If the project has no path yet, it fails with `ap task add failed: no managed path`.

## Workflow commands

The task lifecycle can also be driven explicitly from the CLI.

```bash
ap task sync [project_name]
ap task next [project_name]
ap task set-status <task_id> --status <status> [-p <project_name>]
ap recover [project_name]
ap run start [project_name] [--task <task_id>] [--actor <actor>]
ap run finish <run_id> --status <done|failed|blocked|timeout|cancelled> [--review] [--no-review]
ap review start <coder_run_id> [--actor <actor>]
ap review submit <reviewer_run_id> --verdict <approve|rework|blocked>
```

- `ap task sync` synchronizes `TODO.md` into `runtime/state/tasks`.
- `ap task next` prints the next runnable task as `task_id<TAB>title`.
- `ap recover` finalizes stale in-progress or stale reviewer runs.
- `ap run start` publishes a coder run, moves the task to `in_progress`, and writes `prompt.txt`.
- `ap run finish` finalizes a published coder run without spawning an agent. Use `--review` to move the task to `review_pending` instead of `done`.
- `ap review start` publishes a reviewer run and writes the reviewer prompt.
- `ap review submit` records the verdict and updates task state to `done`, `todo`, or `blocked`.

## Start task

```bash
ap start [project_name]
```

If `<project_name>` is omitted, `ap` asks you to select a project.

`ap start` syncs the target project's `TODO.md` with runtime task state and then selects the first runnable task from state.

`ap start` also requires the target project to have at least one managed path. If none exists, it fails with `ap start failed: no managed path`.

Current `TODO.md` task lines must include a visible stable task ID:

```markdown
- [ ] [demo-0001] Implement ap start command
- [ ] [demo-0002] Add integration tests
```

Completed tasks must use:

```markdown
- [x] [demo-0001] Implement ap start command
```

Notes:

- Task IDs must match `<slug>-NNNN`.
- Duplicate IDs, invalid ID format, slug mismatch, and unknown IDs are hard errors.
- Reordering or renaming a TODO line keeps task identity as long as the ID stays the same.
- If no runnable task exists after sync, `ap start` fails with `ap start failed: no runnable task`.

If `tmux` is available, `ap start` also uses tmux for progress visibility:

- If session `autopilot` does not exist, `ap start` creates it and runs the task in a new worker window.
- If session `autopilot` already exists, `ap start` adds a new worker window to that session.
- Inside tmux, `ap start` switches the current client to the worker window.
- Outside tmux, `ap start` attaches only when it created a fresh `autopilot` session. If the session already exists, it prints the worker window name and waits for completion without attaching.
- When the task finishes, the worker window closes automatically.

If `tmux` is not available, `ap start` falls back to direct execution.

## Briefing tmux session

```bash
ap briefing
```

`ap briefing` creates tmux session `autopilot`, starts Claude in `autopilot:colonel`,
and injects a bootstrap prompt so colonel runs `$ap-self-recognition` first and is ready for `$ap-briefing`.
It then:

- inside tmux: switches to `autopilot:colonel`
- outside tmux: attaches to `autopilot:colonel`
If session `autopilot` already exists, `ap briefing` reuses it. It checks whether window `colonel`
exists and creates it only when missing.

`$briefing` collects and summarizes:

```text
$HOME/.autopilot/projects/<project_name>/dashboard.md
```

and surfaces blockers requiring general instruction/approval.

## Kill tmux session

```bash
ap kill
```

If tmux session `autopilot` exists, `ap kill` terminates it.
If it does not exist, `ap kill` exits successfully without changes.
