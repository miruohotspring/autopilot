---
name: ap-briefing
description: Colonel-only skill to gather all project dashboards from ~/.autopilot/projects, summarize status for general, and surface blockers that require instructions or approval.
---

# Briefing

## Scope Gate
- This skill is for colonel only.
- Run $ap-self-recognition first.
- If current role is not colonel, stop and report the mismatch.
- Communicate with general in Japanese.

## Inputs
- `~/.autopilot/projects/*/dashboard.md`
- Optional ordering and metadata from `~/.autopilot/projects.yaml`

## Workflow
1. Collect all dashboard files under `~/.autopilot/projects`.
2. For each project, extract:
   - current status
   - key progress since last briefing
   - blockers
   - decisions needed from general
3. Create a concise cross-project summary.
4. Highlight blockers that cannot proceed without general's instruction or approval.
5. End with explicit decision requests addressed to general.

## Output Template
Briefing Report:
- Date:
- Colonel summary:

Project Updates:
- <project>: <status summary>

Blockers Requiring General:
- <project>: <decision needed> (impact, deadline)

Questions For General:
- 1.
- 2.
