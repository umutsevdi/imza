#include "platform/credential_subsystem.h"

#include "common/util.h"
#include "platform/command_runner.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <utility>

#if defined(_WIN32)
#include "platform/utf8_convert.h"

#include <wincred.h>
#include <windows.h>
#elif !defined(__APPLE__)
#include <libsecret/secret.h>
#endif

namespace imza {

namespace {

    [[maybe_unused]] constexpr std::string_view PACKAGE = "run.imza.cli";
    constexpr std::string_view SERVICE                  = "imza";
    // Windows Credential Manager caps a blob at 2.5 KiB; the same bound keeps
    // the other backends honest.
    constexpr std::size_t MAX_VALUE_BYTES = 2560;
    // A wedged keyring daemon must not wedge a writer forever.
    constexpr auto PROBE_TIMEOUT = std::chrono::seconds { 10 };

#if defined(__APPLE__)
    constexpr std::string_view SECURITY_BIN  = "/usr/bin/security";
    constexpr std::string_view BASE64_PREFIX = "imza-base64:";
    constexpr auto COMMAND_TIMEOUT           = std::chrono::seconds { 30 };

    // Single-quote for /bin/sh, the form `security` sees through its shell.
    std::string quote(std::string_view text)
    {
        std::string out = "'";
        for (const char character : text) {
            if (character == '\'') {
                out += "'\\''";
            } else {
                out += character;
            }
        }
        return out + "'";
    }
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
    const SecretSchema& schema()
    {
        static const SecretSchema value = [] {
            SecretSchema created { };
            created.name  = PACKAGE.data();
            created.flags = SECRET_SCHEMA_NONE;
            created.attributes[0]
                = { "service", SECRET_SCHEMA_ATTRIBUTE_STRING };
            created.attributes[1]
                = { "username", SECRET_SCHEMA_ATTRIBUTE_STRING };
            return created;
        }();
        return value;
    }
#endif

    // True when the platform store is usable; `reason` explains a failure.
    bool probe_backend(std::string& reason)
    {
#if defined(_WIN32)
        // Credential Manager is part of every supported Windows install.
        return true;
#elif defined(__APPLE__)
        if (!std::filesystem::exists(SECURITY_BIN)) {
            reason = "/usr/bin/security is missing";
            return false;
        }
        const CommandResult probe = run_command(
            std::string(SECURITY_BIN) + " list-keychains", COMMAND_TIMEOUT);
        if (!command_ok(probe)) {
            reason = "the macOS keychain is unavailable";
            return false;
        }
        return true;
#else
        // OPEN_SESSION is what makes libsecret contact org.freedesktop.secrets:
        // without it the call only builds the D-Bus proxy, which succeeds on
        // any machine that merely has a session bus.
        GError* error          = nullptr;
        SecretService* service = secret_service_get_sync(
            SECRET_SERVICE_OPEN_SESSION, nullptr, &error);
        if (error != nullptr || service == nullptr) {
            reason = error != nullptr && error->message != nullptr
                ? error->message
                : "the Secret Service is unavailable";
            if (error != nullptr) {
                g_error_free(error);
            }
            return false;
        }
        g_object_unref(service);
        return true;
#endif
    }

    std::string read_backend(const std::string& key, Status& status)
    {
        status = Status::OK;
#if defined(_WIN32)
        const std::wstring target = to_wide(
            std::string(PACKAGE) + "." + std::string(SERVICE) + "/" + key);
        PCREDENTIALW credential = nullptr;
        if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
            status = GetLastError() == ERROR_NOT_FOUND ? Status::NOT_FOUND
                                                       : Status::CONFIG_ERROR;
            return { };
        }
        std::string value(
            reinterpret_cast<const char*>(credential->CredentialBlob),
            credential->CredentialBlobSize);
        CredFree(credential);
        return value;
#elif defined(__APPLE__)
        const CommandResult result = run_command(std::string(SECURITY_BIN)
                + " find-generic-password -s " + quote(SERVICE) + " -a "
                + quote(key) + " -w",
            COMMAND_TIMEOUT);
        if (!command_ok(result)) {
            status
                = result.output.find("could not be found") != std::string::npos
                ? Status::NOT_FOUND
                : Status::CONFIG_ERROR;
            return { };
        }
        const std::string stored = std::string(trim(result.output));
        if (stored.starts_with(BASE64_PREFIX)) {
            const std::optional<std::string> decoded
                = base64_decode(stored.substr(BASE64_PREFIX.size()));
            if (!decoded) {
                status = Status::CONFIG_ERROR;
                return { };
            }
            return *decoded;
        }
        return stored;
#else
        GError* error = nullptr;
        gchar* secret
            = secret_password_lookup_sync(&schema(), nullptr, &error, "service",
                std::string(SERVICE).c_str(), "username", key.c_str(), nullptr);
        if (error != nullptr) {
            status = Status::CONFIG_ERROR;
            g_error_free(error);
            return { };
        }
        if (secret == nullptr) {
            status = Status::NOT_FOUND;
            return { };
        }
        std::string value(secret);
        secret_password_free(secret);
        return value;
#endif
    }

    Status write_backend(const std::string& key, const std::string& value)
    {
        if (value.size() > MAX_VALUE_BYTES) {
            return Status::CONFIG_ERROR;
        }
#if defined(_WIN32)
        const std::wstring target = to_wide(
            std::string(PACKAGE) + "." + std::string(SERVICE) + "/" + key);
        const std::wstring user = to_wide(key);
        CREDENTIALW credential { };
        credential.Type               = CRED_TYPE_GENERIC;
        credential.TargetName         = const_cast<LPWSTR>(target.c_str());
        credential.UserName           = const_cast<LPWSTR>(user.c_str());
        credential.CredentialBlobSize = static_cast<DWORD>(value.size());
        credential.CredentialBlob
            = reinterpret_cast<LPBYTE>(const_cast<char*>(value.data()));
        credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
        return CredWriteW(&credential, 0) ? Status::OK : Status::CONFIG_ERROR;
#elif defined(__APPLE__)
        // The value travels on stdin: `security -i` reads commands from there,
        // so the secret never appears in the process table.
        const std::string command = "add-generic-password -U -s "
            + quote(SERVICE) + " -a " + quote(key) + " -w "
            + quote(std::string(BASE64_PREFIX) + base64_encode(value)) + "\n";
        const CommandResult result = run_command(
            std::string(SECURITY_BIN) + " -i", COMMAND_TIMEOUT, { }, command);
        return command_ok(result) ? Status::OK : Status::CONFIG_ERROR;
#else
        GError* error = nullptr;
        const gboolean stored
            = secret_password_store_sync(&schema(), SECRET_COLLECTION_DEFAULT,
                (std::string(SERVICE) + " (" + key + ")").c_str(),
                value.c_str(), nullptr, &error, "service",
                std::string(SERVICE).c_str(), "username", key.c_str(), nullptr);
        if (error != nullptr) {
            g_error_free(error);
            return Status::CONFIG_ERROR;
        }
        return stored ? Status::OK : Status::CONFIG_ERROR;
#endif
    }

    Status delete_backend(const std::string& key)
    {
#if defined(_WIN32)
        const std::wstring target = to_wide(
            std::string(PACKAGE) + "." + std::string(SERVICE) + "/" + key);
        if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
            return GetLastError() == ERROR_NOT_FOUND ? Status::NOT_FOUND
                                                     : Status::CONFIG_ERROR;
        }
        return Status::OK;
#elif defined(__APPLE__)
        const CommandResult result = run_command(std::string(SECURITY_BIN)
                + " delete-generic-password -s " + quote(SERVICE) + " -a "
                + quote(key),
            COMMAND_TIMEOUT);
        if (!command_ok(result)) {
            return result.output.find("could not be found") != std::string::npos
                ? Status::NOT_FOUND
                : Status::CONFIG_ERROR;
        }
        return Status::OK;
#else
        GError* error = nullptr;
        const gboolean removed
            = secret_password_clear_sync(&schema(), nullptr, &error, "service",
                std::string(SERVICE).c_str(), "username", key.c_str(), nullptr);
        if (error != nullptr) {
            g_error_free(error);
            return Status::CONFIG_ERROR;
        }
        return removed ? Status::OK : Status::NOT_FOUND;
#endif
    }

} // namespace

CredentialSubsystem::CredentialSubsystem(Mode mode,
    std::vector<Connection> connections, std::filesystem::path config_path)
{
    // An explicit choice is not a degradation: no reason, no notice, and
    // nothing to probe, so both latches settle here and no worker starts.
    if (mode == Mode::PLAINTEXT) {
        _probed.store(true);
        _ready.store(true);
        return;
    }
    _worker = std::jthread(
        [this, connections = std::move(connections),
            path = std::move(config_path)](std::stop_token stop) {
            _probe();
            if (stop.stop_requested()) {
                return;
            }
            if (_keychain.load() && !connections.empty()) {
                _resolve(connections);
                if (!path.empty() && !stop.stop_requested()) {
                    // The document is re-read here: the caller's copy may
                    // already be stale by the time the worker runs.
                    Config document;
                    if (load_config(path, document) == Status::OK) {
                        resolve_credentials(document, *this);
                        adopt_legacy_credentials(path, document, *this);
                    }
                }
            }
            if (stop.stop_requested()) {
                return;
            }
            // Announce before flipping the latch: a subscriber that sees
            // ready() must already have received the startup announcement,
            // or it would observe the publish as a later change.
            _generation.fetch_add(1);
            _changed.publish();
            _ready.store(true);
            _ready_cv.notify_all();
        });
}

CredentialSubsystem::~CredentialSubsystem() { stop(); }

void CredentialSubsystem::stop()
{
    mark_dead();
    if (_worker.joinable()) {
        _worker.request_stop();
        _worker.join();
    }
    // A worker cut short still unblocks every waiter on either latch.
    _probed.store(true);
    _probe_cv.notify_all();
    _ready.store(true);
    _ready_cv.notify_all();
}

void CredentialSubsystem::_probe()
{
    std::string reason;
    const bool available = probe_backend(reason);
    {
        std::lock_guard lock(_mutex);
        _keychain = available;
        _reason   = available ? std::string { } : std::move(reason);
    }
    _probed.store(true);
    _probe_cv.notify_all();
}

void CredentialSubsystem::_resolve(const std::vector<Connection>& connections)
{
    std::unordered_map<std::string, std::string> resolved;
    for (const Connection& connection : connections) {
        Status status      = Status::OK;
        std::string stored = read_backend(credential_key(connection), status);
        if (status == Status::OK) {
            resolved.emplace(credential_key(connection), std::move(stored));
        }
    }
    std::lock_guard lock(_mutex);
    for (auto& [key, value] : resolved) {
        _secrets.insert_or_assign(std::move(key), std::move(value));
    }
}

bool CredentialSubsystem::_await_probe() const
{
    if (_probed.load()) {
        return true;
    }
    std::unique_lock lock(_mutex);
    return _probe_cv.wait_for(
        lock, PROBE_TIMEOUT, [this] { return _probed.load(); });
}

bool CredentialSubsystem::is_keychain_available() const
{
    _await_probe();
    return _keychain.load();
}

std::string CredentialSubsystem::unavailable_reason() const
{
    _await_probe();
    std::lock_guard lock(_mutex);
    return _reason;
}

bool CredentialSubsystem::wait_ready(std::chrono::milliseconds timeout) const
{
    if (_ready.load()) {
        return true;
    }
    std::unique_lock lock(_mutex);
    _ready_cv.wait_for(lock, timeout, [this] { return _ready.load(); });
    return _ready.load();
}

std::string CredentialSubsystem::get(
    const std::string& key, Status& status) const
{
    if (!_keychain.load()) {
        status = Status::NOT_FOUND;
        return { };
    }
    return read_backend(key, status);
}

std::string CredentialSubsystem::lookup(
    const std::string& key, Status& status) const
{
    {
        std::lock_guard lock(_mutex);
        const auto found = _secrets.find(key);
        if (found != _secrets.end()) {
            status = Status::OK;
            return found->second;
        }
    }
    return get(key, status);
}

std::optional<std::string> CredentialSubsystem::cached_secret(
    std::string_view key) const
{
    std::lock_guard lock(_mutex);
    const auto found = _secrets.find(std::string(key));
    if (found == _secrets.end()) {
        return std::nullopt;
    }
    return found->second;
}

Status CredentialSubsystem::set(
    const std::string& key, const std::string& value)
{
    if (!_keychain.load()) {
        return Status::CONFIG_ERROR;
    }
    const Status status = write_backend(key, value);
    if (status != Status::OK) {
        return status;
    }
    {
        std::lock_guard lock(_mutex);
        _secrets.insert_or_assign(key, value);
    }
    _generation.fetch_add(1);
    _changed.publish();
    return Status::OK;
}

Status CredentialSubsystem::remove(const std::string& key)
{
    if (!_keychain.load()) {
        return Status::CONFIG_ERROR;
    }
    const Status status = delete_backend(key);
    if (status != Status::OK) {
        return status;
    }
    {
        std::lock_guard lock(_mutex);
        _secrets.erase(key);
    }
    _generation.fetch_add(1);
    _changed.publish();
    return Status::OK;
}

Status CredentialSubsystem::has(const std::string& key) const
{
    Status status = Status::OK;
    lookup(key, status);
    return status;
}

Signal<>::Subscription CredentialSubsystem::subscribe_to_change(
    Signal<>::Callback callback)
{
    return _changed.subscribe(std::move(callback));
}

} // namespace imza
