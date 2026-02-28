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
