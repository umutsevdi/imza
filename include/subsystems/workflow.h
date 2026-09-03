#pragma once

#include <functional>
#include <optional>

#include "subsystems/session.h"

namespace ursa {

enum class WorkflowPhase { PLAN, BUILD, REVIEW };

WorkflowPhase next_workflow_phase(WorkflowPhase phase, bool review_available);
WorkflowPhase previous_workflow_phase(
    WorkflowPhase phase, bool review_available);
std::optional<Session::Mode> workflow_mode(WorkflowPhase phase);

using WorkflowFn         = std::function<WorkflowPhase()>;
using WorkflowNavigateFn = std::function<void(WorkflowPhase)>;

} // namespace ursa
