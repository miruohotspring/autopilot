#include "autopilot/commands/cmd_init.hpp"
#include "autopilot/platform/home_dir.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

void write_text_file(const fs::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    throw std::runtime_error("failed to create file: " + path.string());
  }
  out << content;
  if (!out) {
    throw std::runtime_error("failed to write file: " + path.string());
  }
}

const char* kClaudeMd = R"(# Autopilot Command Structure

## Roles
- general: the user. General gives direction and approvals.
- colonel: orchestrates all projects and manages captains.
- captain: development lead for one project.

## Constraints
- colonel and captain do not implement production tasks directly.
- They only coordinate, report, ask for decisions, and route work.
- colonel communicates with general in Japanese.

## Session policy
- Invoke $self-recognition at the beginning of each session.
- Use $briefing only when acting as colonel.
)";

const char* kSelfRecognitionSkill = R"(---
name: self-recognition
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
)";

const char* kSelfRecognitionOpenAiYaml = R"(interface:
  display_name: "Self Recognition"
  short_description: "Check role and pending tasks before action"
  default_prompt: "Use $self-recognition to identify the active role and pending tasks."
)";

const char* kBriefingSkill = R"(---
name: briefing
description: Colonel-only skill to gather all project dashboards from ~/.autopilot/projects, summarize status for general, and surface blockers that require instructions or approval.
---

# Briefing

## Scope Gate
- This skill is for colonel only.
- Run $self-recognition first.
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
)";

const char* kBriefingOpenAiYaml = R"(interface:
  display_name: "Briefing"
  short_description: "Summarize project dashboards for general"
  default_prompt: "Use $briefing to prepare a colonel report from all dashboards."
)";

} // namespace

int cmd_init() {
  const fs::path home = get_home_dir();
  const fs::path autopilot_dir = home / ".autopilot";
  const fs::path projects_dir = autopilot_dir / "projects";
  const fs::path claude_skills_dir = autopilot_dir / ".claude" / "skills";
  const fs::path backup_dir = home / ".autopilot.bak";

  try {
    if (fs::exists(autopilot_dir)) {
      if (fs::exists(backup_dir)) {
        std::cerr << "ap init failed: backup already exists: " << backup_dir << '\n';
        return 1;
      }
      fs::rename(autopilot_dir, backup_dir);
      std::cout << "renamed: " << autopilot_dir << " -> " << backup_dir << '\n';
    }

    fs::create_directories(projects_dir);
    fs::create_directories(claude_skills_dir / "self-recognition" / "agents");
    fs::create_directories(claude_skills_dir / "briefing" / "agents");

    write_text_file(autopilot_dir / "CLAUDE.md", kClaudeMd);
    write_text_file(claude_skills_dir / "self-recognition" / "SKILL.md", kSelfRecognitionSkill);
    write_text_file(
        claude_skills_dir / "self-recognition" / "agents" / "openai.yaml",
        kSelfRecognitionOpenAiYaml);
    write_text_file(claude_skills_dir / "briefing" / "SKILL.md", kBriefingSkill);
    write_text_file(
        claude_skills_dir / "briefing" / "agents" / "openai.yaml",
        kBriefingOpenAiYaml);

    std::cout << "created: " << autopilot_dir << '\n';
    std::cout << "created: " << projects_dir << '\n';
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ap init failed: " << e.what() << '\n';
    return 1;
  }
}
