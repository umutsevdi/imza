#include <doctest/doctest.h>

#include "subsystems/workflow.h"

TEST_CASE("workflow advances through plan build and review")
{
    using ursa::WorkflowPhase;

    CHECK(ursa::next_workflow_phase(WorkflowPhase::PLAN, true)
        == WorkflowPhase::BUILD);
    CHECK(ursa::next_workflow_phase(WorkflowPhase::BUILD, true)
        == WorkflowPhase::REVIEW);
    CHECK(ursa::next_workflow_phase(WorkflowPhase::REVIEW, true)
        == WorkflowPhase::PLAN);
}

TEST_CASE("workflow reverses through plan review and build")
{
    using ursa::WorkflowPhase;

    CHECK(ursa::previous_workflow_phase(WorkflowPhase::PLAN, true)
        == WorkflowPhase::REVIEW);
    CHECK(ursa::previous_workflow_phase(WorkflowPhase::REVIEW, true)
        == WorkflowPhase::BUILD);
    CHECK(ursa::previous_workflow_phase(WorkflowPhase::BUILD, true)
        == WorkflowPhase::PLAN);
}

TEST_CASE("workflow skips review when no git workspace is available")
{
    using ursa::WorkflowPhase;

    CHECK(ursa::next_workflow_phase(WorkflowPhase::PLAN, false)
        == WorkflowPhase::BUILD);
    CHECK(ursa::next_workflow_phase(WorkflowPhase::BUILD, false)
        == WorkflowPhase::PLAN);
    CHECK(ursa::previous_workflow_phase(WorkflowPhase::PLAN, false)
        == WorkflowPhase::BUILD);
    CHECK(ursa::previous_workflow_phase(WorkflowPhase::BUILD, false)
        == WorkflowPhase::PLAN);
}

TEST_CASE("only conversational workflow phases select an agent mode")
{
    using ursa::Session;
    using ursa::WorkflowPhase;

    CHECK(ursa::workflow_mode(WorkflowPhase::PLAN) == Session::Mode::PLAN);
    CHECK(ursa::workflow_mode(WorkflowPhase::BUILD) == Session::Mode::BUILD);
    CHECK_FALSE(ursa::workflow_mode(WorkflowPhase::REVIEW).has_value());
}
