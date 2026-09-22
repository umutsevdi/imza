#include <doctest/doctest.h>

#include "conversation/session.h"

TEST_CASE("workflow phase navigation")
{
    using imza::Session;
    using imza::WorkflowPhase;

    // Forward and reverse traversal with a git workspace wraps through review.
    CHECK(imza::next_workflow_phase(WorkflowPhase::PLAN, true)
        == WorkflowPhase::BUILD);
    CHECK(imza::next_workflow_phase(WorkflowPhase::BUILD, true)
        == WorkflowPhase::REVIEW);
    CHECK(imza::next_workflow_phase(WorkflowPhase::REVIEW, true)
        == WorkflowPhase::PLAN);
    CHECK(imza::previous_workflow_phase(WorkflowPhase::PLAN, true)
        == WorkflowPhase::REVIEW);
    CHECK(imza::previous_workflow_phase(WorkflowPhase::REVIEW, true)
        == WorkflowPhase::BUILD);
    CHECK(imza::previous_workflow_phase(WorkflowPhase::BUILD, true)
        == WorkflowPhase::PLAN);

    // Without git, review is skipped in both directions.
    CHECK(imza::next_workflow_phase(WorkflowPhase::PLAN, false)
        == WorkflowPhase::BUILD);
    CHECK(imza::next_workflow_phase(WorkflowPhase::BUILD, false)
        == WorkflowPhase::PLAN);
    CHECK(imza::previous_workflow_phase(WorkflowPhase::PLAN, false)
        == WorkflowPhase::BUILD);
    CHECK(imza::previous_workflow_phase(WorkflowPhase::BUILD, false)
        == WorkflowPhase::PLAN);

    // Only conversational phases select an agent mode.
    CHECK(imza::workflow_mode(WorkflowPhase::PLAN) == Session::Mode::PLAN);
    CHECK(imza::workflow_mode(WorkflowPhase::BUILD) == Session::Mode::BUILD);
    CHECK_FALSE(imza::workflow_mode(WorkflowPhase::REVIEW).has_value());
}
