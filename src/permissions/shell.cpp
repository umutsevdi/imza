#include "permissions/shell.h"

#include <algorithm>
#include <string>
#include <utility>

#include "permissions/shell_analysis.h"

namespace imza {

namespace {

    bool has_shell_program(
        const PermissionStore::Grants& grants, std::string_view program)
    {
        return std::any_of(
            grants.begin(), grants.end(), [&](const auto& grant) {
                const auto* shell = std::get_if<ShellCommandGrant>(&grant);
                return shell != nullptr && shell->program == program;
            });
    }

    std::string shell_grant_reason(const PermissionStore::Grants& grants)
    {
        std::string reason
            = "shell commands require approval · session scope: ";
        for (const PermissionGrant& grant : grants) {
            const auto* shell = std::get_if<ShellCommandGrant>(&grant);
            if (shell == nullptr) {
                continue;
            }
            if (!reason.ends_with(": ")) {
                reason += ", ";
            }
            reason += shell->program + " " + shell->subcommand.value_or("*");
        }
        return reason;
    }

    // Connecting spawns a process or opens a network session, so an
    // offline server asks. The ask carries no session grant on purpose:

} // namespace

ShellEvaluation evaluate_shell_request(
    ShellRequest request, const PermissionContext& context)
{
    if (request.command.empty()) {
        return { { PermissionDecision::Kind::REJECT,
                     "'command' must be a non-empty string" },
            std::move(request), { } };
    }
    if (!context.grants) {
        return { { PermissionDecision::Kind::REJECT,
                     "permission grants are unavailable" },
            std::move(request), { } };
    }
    const ShellAnalysis analysis = analyze_shell(request.command);
    // Deletion programs are rejected outright, before grants: deletion
    // goes through imza.fs.remove so it stays behind the filesystem gate.
    for (const ShellInvocation& invocation : analysis.invocations) {
        if (!shell_deletion_denied(invocation.program)) {
            continue;
        }
        std::string guidance = "'" + invocation.program
            + "' is not permitted; use imza.fs.remove(path) to delete a file "
              "or an empty directory";
        return { { PermissionDecision::Kind::REJECT, std::move(guidance) },
            std::move(request), { } };
    }
    if (analysis.reuse == ShellAnalysis::Reuse::ONCE) {
        return { { PermissionDecision::Kind::ASK,
                     "shell syntax requires approval for each execution" },
            std::move(request), { } };
    }

    PermissionStore::Grants candidates;
    for (const ShellInvocation& invocation : analysis.invocations) {
        if (shell_builtin_allowed(invocation.program)
            || shell_readonly_allowed(invocation)) {
            continue;
        }
        const ShellCommandGrant requested { invocation.program,
            invocation.subcommand };
        if (grants_cover(context.grants, requested)) {
            continue;
        }
        ShellCommandGrant candidate = requested;
        if (has_shell_program(*context.grants, requested.program)) {
            candidate.subcommand.reset();
        }
        for (PermissionGrant& pending : candidates) {
            auto* shell = std::get_if<ShellCommandGrant>(&pending);
            if (shell == nullptr || shell->program != candidate.program) {
                continue;
            }
            if (shell->subcommand != candidate.subcommand) {
                shell->subcommand.reset();
            }
            candidate.program.clear();
            break;
        }
        if (!candidate.program.empty()) {
            candidates.emplace_back(std::move(candidate));
        }
    }
    if (candidates.empty()) {
        return { { PermissionDecision::Kind::ACCEPT, "" }, std::move(request),
            { } };
    }
    const std::string reason = shell_grant_reason(candidates);
    return { { PermissionDecision::Kind::ASK, reason }, std::move(request),
        std::move(candidates) };
}

} // namespace imza
