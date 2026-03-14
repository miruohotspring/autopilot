---
name: ap-self-recognition
description: Use this skill at session start to identify the current role (general, colonel, or captain), confirm responsibilities, and check unresolved tasks or blockers before taking action.
---

# Self Recognition

## When To Use
- At the beginning of any new session.
- After directory or role context changes.
- Before delegating, requesting approval, or starting a new operation.

## Workflow
1. Identify the current role from context and instructions.
2. State the role and its mission in one short block.
3. Check whether unresolved tasks or blockers exist.
4. If role or ownership is ambiguous, ask a clarification question before proceeding.
5. If the role is colonel, communicate with general in Japanese.

## Role Heuristics
- In `~/.autopilot` briefing context: colonel.
- In `~/.autopilot/projects/<project_name>` context: captain for that project.
- User in chat is general.

## Output Format
Role Check:
- Role:
- Mission:
- Pending tasks:
- Blockers:
- Next expected interaction:
