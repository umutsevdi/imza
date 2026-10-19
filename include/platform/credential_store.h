#pragma once

#include <string>
#include <string_view>

#include "common/imza_signal.h"
#include "common/types.h"

namespace imza {

// OS keychain access for provider secrets. The backend is chosen once, in
// the constructor, and never changes: Mode::OS probes the platform store and
// falls back to Mode::PLAINTEXT (recording why) when it is unusable, so a
// machine without a keyring still runs. Every value is an opaque string; the
// caller owns its meaning.
class CredentialStore final : public Subsystem {
public:
    enum class Mode { OS, PLAINTEXT };

    explicit CredentialStore(Mode mode = Mode::OS);
    ~CredentialStore() override = default;

    CredentialStore(const CredentialStore&)            = delete;
    CredentialStore& operator=(const CredentialStore&) = delete;

    // The stored value; empty with Status::NOT_FOUND when absent, and
    // CONFIG_ERROR when the keychain call failed.
    std::string get(const std::string& key, Status& status) const;
    Status set(const std::string& key, const std::string& value);
    Status remove(const std::string& key);
    Status has(const std::string& key) const;

    // False in Mode::PLAINTEXT and after a failed probe. `reason` explains a
    // failed probe and is empty for an explicit plaintext choice, so startup
    // only warns about the former. Both are fixed for the process lifetime.
    bool is_keychain_available() const { return _keychain; }
    const std::string& unavailable_reason() const { return _reason; }

    // Callbacks run on the publishing thread; keep the Subscription.
    [[nodiscard]] Signal<>::Subscription subscribe_to_change(
        Signal<>::Callback callback);

private:
    bool _keychain = false;
    std::string _reason;
    Signal<> _changed;
};

} // namespace imza
