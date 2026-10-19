#include "platform/credential_store.h"

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

    constexpr std::string_view PACKAGE = "run.imza.cli";
    constexpr std::string_view SERVICE = "imza";
    // Windows Credential Manager caps a blob at 2.5 KiB; the same bound keeps
    // the other backends honest.
    constexpr std::size_t MAX_VALUE_BYTES = 2560;

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

} // namespace

CredentialStore::CredentialStore(Mode mode)
{
    // An explicit choice is not a degradation, so it carries no reason and
    // startup shows no notice.
    if (mode == Mode::PLAINTEXT) {
        return;
    }
#if defined(_WIN32)
    // Credential Manager is part of every supported Windows install.
    _keychain = true;
#elif defined(__APPLE__)
    if (!std::filesystem::exists(SECURITY_BIN)) {
        _reason = "/usr/bin/security is missing";
        return;
    }
    const CommandResult probe = run_command(
        std::string(SECURITY_BIN) + " list-keychains", COMMAND_TIMEOUT);
    if (!command_ok(probe)) {
        _reason = "the macOS keychain is unavailable";
        return;
    }
    _keychain = true;
#else
    GError* error          = nullptr;
    SecretService* service = secret_service_get_sync(
        static_cast<SecretServiceFlags>(0), nullptr, &error);
    if (error != nullptr || service == nullptr) {
        _reason = error != nullptr && error->message != nullptr
            ? error->message
            : "the Secret Service is unavailable";
        if (error != nullptr) {
            g_error_free(error);
        }
        return;
    }
    g_object_unref(service);
    _keychain = true;
#endif
}

std::string CredentialStore::get(const std::string& key, Status& status) const
{
    status = Status::OK;
    if (!_keychain) {
        status = Status::NOT_FOUND;
        return { };
    }
#if defined(_WIN32)
    const std::wstring target = to_wide(
        std::string(PACKAGE) + "." + std::string(SERVICE) + "/" + key);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        status = GetLastError() == ERROR_NOT_FOUND ? Status::NOT_FOUND
                                                   : Status::CONFIG_ERROR;
        return { };
    }
    std::string value(reinterpret_cast<const char*>(credential->CredentialBlob),
        credential->CredentialBlobSize);
    CredFree(credential);
    return value;
#elif defined(__APPLE__)
    const CommandResult result
        = run_command(std::string(SECURITY_BIN) + " find-generic-password -s "
                + quote(SERVICE) + " -a " + quote(key) + " -w",
            COMMAND_TIMEOUT);
    if (!command_ok(result)) {
        status = result.output.find("could not be found") != std::string::npos
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

Status CredentialStore::set(const std::string& key, const std::string& value)
{
    if (!_keychain) {
        return Status::CONFIG_ERROR;
    }
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
    if (!CredWriteW(&credential, 0)) {
        return Status::CONFIG_ERROR;
    }
#elif defined(__APPLE__)
    // The value travels on stdin: `security -i` reads commands from there, so
    // the secret never appears in the process table.
    const std::string command = "add-generic-password -U -s " + quote(SERVICE)
        + " -a " + quote(key) + " -w "
        + quote(std::string(BASE64_PREFIX) + base64_encode(value)) + "\n";
    const CommandResult result = run_command(
        std::string(SECURITY_BIN) + " -i", COMMAND_TIMEOUT, { }, command);
    if (!command_ok(result)) {
        return Status::CONFIG_ERROR;
    }
#else
    GError* error = nullptr;
    const gboolean stored
        = secret_password_store_sync(&schema(), SECRET_COLLECTION_DEFAULT,
            (std::string(SERVICE) + " (" + key + ")").c_str(), value.c_str(),
            nullptr, &error, "service", std::string(SERVICE).c_str(),
            "username", key.c_str(), nullptr);
    if (error != nullptr) {
        g_error_free(error);
        return Status::CONFIG_ERROR;
    }
    if (!stored) {
        return Status::CONFIG_ERROR;
    }
#endif
    _changed.publish();
    return Status::OK;
}

Status CredentialStore::remove(const std::string& key)
{
    if (!_keychain) {
        return Status::CONFIG_ERROR;
    }
#if defined(_WIN32)
    const std::wstring target = to_wide(
        std::string(PACKAGE) + "." + std::string(SERVICE) + "/" + key);
    if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
        return GetLastError() == ERROR_NOT_FOUND ? Status::NOT_FOUND
                                                 : Status::CONFIG_ERROR;
    }
#elif defined(__APPLE__)
    const CommandResult result
        = run_command(std::string(SECURITY_BIN) + " delete-generic-password -s "
                + quote(SERVICE) + " -a " + quote(key),
            COMMAND_TIMEOUT);
    if (!command_ok(result)) {
        return result.output.find("could not be found") != std::string::npos
            ? Status::NOT_FOUND
            : Status::CONFIG_ERROR;
    }
#else
    GError* error = nullptr;
    const gboolean removed
        = secret_password_clear_sync(&schema(), nullptr, &error, "service",
            std::string(SERVICE).c_str(), "username", key.c_str(), nullptr);
    if (error != nullptr) {
        g_error_free(error);
        return Status::CONFIG_ERROR;
    }
    if (!removed) {
        return Status::NOT_FOUND;
    }
#endif
    _changed.publish();
    return Status::OK;
}

Status CredentialStore::has(const std::string& key) const
{
    Status status = Status::OK;
    get(key, status);
    return status;
}

Signal<>::Subscription CredentialStore::subscribe_to_change(
    Signal<>::Callback callback)
{
    return _changed.subscribe(std::move(callback));
}

} // namespace imza
