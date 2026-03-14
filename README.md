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
It also creates Claude Code project skills in:

```text
$HOME/.autopilot/.claude/skills/self-recognition
$HOME/.autopilot/.claude/skills/briefing
```

Run this command from the autopilot repository root.

## Update managed files

```bash
ap update
```

`ap update` copies managed files from this repository's `templates/autopilot` into
`$HOME/.autopilot`.

Run this command from the autopilot repository root.

## New project

```bash
ap new <project_name>
```

If `<project_name>` is omitted, `ap` asks interactively:

```text
Enter your new project name:
```

`project_name` can include letters, numbers, and hyphens (`-`), and cannot start or end with a hyphen.

`ap new` adds project entries with this structure:

```yaml
<project_name>:
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

`ap add` stores both `name` and `path` in `projects.yaml`.

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

## Briefing tmux session

```bash
ap briefing
```

`ap briefing` creates tmux session `autopilot`, starts Claude in `autopilot:colonel`,
and injects a bootstrap prompt so colonel runs `$self-recognition` first and is ready for `$briefing`.
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
