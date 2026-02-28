# autopilot

## Build

```bash
make
```

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
