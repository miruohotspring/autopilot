#pragma once

#include <optional>
#include <string>
#include <vector>

int cmd_task_sync(const std::optional<std::string>& maybe_project_name);
int cmd_task_next(const std::optional<std::string>& maybe_project_name);
int cmd_task_set_status(
    const std::optional<std::string>& maybe_project_name,
    const std::string& task_id,
    const std::string& status);
int cmd_recover(const std::optional<std::string>& maybe_project_name);
int cmd_run_start(
    const std::optional<std::string>& maybe_project_name,
    const std::optional<std::string>& maybe_task_id,
    const std::optional<std::string>& maybe_actor_name);
int cmd_run_finish(
    const std::string& run_id,
    const std::string& status,
    std::optional<bool> maybe_review_enabled,
    const std::optional<std::string>& maybe_summary,
    const std::optional<std::string>& maybe_blocker_reason,
    const std::optional<std::string>& maybe_blocker_category,
    bool approval_required);
int cmd_review_start(
    const std::string& coder_run_id,
    const std::optional<std::string>& maybe_actor_name);
int cmd_review_submit(
    const std::string& reviewer_run_id,
    const std::string& verdict,
    const std::optional<std::string>& maybe_summary,
    const std::vector<std::string>& issues,
    const std::vector<std::string>& suggestions,
    const std::optional<std::string>& maybe_reason,
    const std::optional<std::string>& maybe_category);
