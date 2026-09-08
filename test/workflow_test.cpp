#include <doctest/doctest.h>

#include "conversation/workflow.h"

TEST_CASE("workflow advances through plan build and review")
{
    using imza::WorkflowPhase;

    CHECK(imza::next_workflow_phase(WorkflowPhase::PLAN, true)
        == WorkflowPhase::BUILD);
    CHECK(imza::next_workflow_phase(WorkflowPhase::BUILD, true)
        == WorkflowPhase::REVIEW);
    CHECK(imza::next_workflow_phase(WorkflowPhase::REVIEW, true)
        == WorkflowPhase::PLAN);
}

TEST_CASE("workflow reverses through plan review and build")
{
    using imza::WorkflowPhase;

    CHECK(imza::previous_workflow_phase(WorkflowPhase::PLAN, true)
        == WorkflowPhase::REVIEW);
    CHECK(imza::previous_workflow_phase(WorkflowPhase::REVIEW, true)
        == WorkflowPhase::BUILD);
    CHECK(imza::previous_workflow_phase(WorkflowPhase::BUILD, true)
        == WorkflowPhase::PLAN);
}

TEST_CASE("workflow skips review when no git workspace is available")
{
    using imza::WorkflowPhase;

    CHECK(imza::next_workflow_phase(WorkflowPhase::PLAN, false)
        == WorkflowPhase::BUILD);
    CHECK(imza::next_workflow_phase(WorkflowPhase::BUILD, false)
        == WorkflowPhase::PLAN);
    CHECK(imza::previous_workflow_phase(WorkflowPhase::PLAN, false)
        == WorkflowPhase::BUILD);
    CHECK(imza::previous_workflow_phase(WorkflowPhase::BUILD, false)
        == WorkflowPhase::PLAN);
}

TEST_CASE("only conversational workflow phases select an agent mode")
{
    using imza::Session;
    using imza::WorkflowPhase;

    CHECK(imza::workflow_mode(WorkflowPhase::PLAN) == Session::Mode::PLAN);
    CHECK(imza::workflow_mode(WorkflowPhase::BUILD) == Session::Mode::BUILD);
    CHECK_FALSE(imza::workflow_mode(WorkflowPhase::REVIEW).has_value());
}
