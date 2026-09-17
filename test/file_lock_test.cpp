#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "platform/file_lock.h"

namespace {

std::filesystem::path temp_path(const std::string& name)
{
    static int counter = 0;
    auto dir           = std::filesystem::temp_directory_path()
        / ("imza-file-lock-test-" + std::to_string(counter++));
    std::filesystem::create_directories(dir);
    return dir / name;
}

} // namespace

TEST_CASE("acquired lock blocks a second holder until released")
{
#ifdef _WIN32
    return;
#else
    const auto path = temp_path("mutex.lock");
    auto first      = imza::acquire_file_lock(path);
    REQUIRE(std::holds_alternative<imza::FileLock>(first));

    std::atomic<bool> acquired { false };
    std::thread second([&] {
        auto lock = imza::acquire_file_lock(path);
        acquired  = std::holds_alternative<imza::FileLock>(lock);
    });

    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        if (acquired) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_FALSE(acquired);

    first = std::variant<imza::FileLock, imza::FileLockError> {
        imza::FileLockError { }
    };
    second.join();
    CHECK(acquired);
#endif
}

TEST_CASE("lock file is removed on release and can be reacquired")
{
#ifdef _WIN32
    return;
#else
    const auto path = temp_path("persist.lock");
    {
        auto lock = imza::acquire_file_lock(path);
        REQUIRE(std::holds_alternative<imza::FileLock>(lock));
    }
    CHECK_FALSE(std::filesystem::exists(path));
    {
        auto lock = imza::acquire_file_lock(path);
        REQUIRE(std::holds_alternative<imza::FileLock>(lock));
    }
    CHECK_FALSE(std::filesystem::exists(path));
#endif
}

TEST_CASE("non-blocking request fails while another holder is live")
{
#ifdef _WIN32
    return;
#else
    const auto path = temp_path("try.lock");
    auto first      = imza::acquire_file_lock(path);
    REQUIRE(std::holds_alternative<imza::FileLock>(first));

    auto second = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    CHECK(std::holds_alternative<imza::FileLockError>(second));

    CHECK(imza::file_lock_held(path));

    first = std::variant<imza::FileLock, imza::FileLockError> {
        imza::FileLockError { }
    };
    CHECK_FALSE(imza::file_lock_held(path));

    auto third = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    CHECK(std::holds_alternative<imza::FileLock>(third));
    // The probe reports any holder, including this process.
    CHECK(imza::file_lock_held(path));
#endif
}

TEST_CASE("shared locks coexist and yield to an exclusive holder")
{
#ifdef _WIN32
    return;
#else
    const auto path = temp_path("shared.lock");
    auto first      = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::SHARED, false });
    auto second = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::SHARED, false });
    REQUIRE(std::holds_alternative<imza::FileLock>(first));
    REQUIRE(std::holds_alternative<imza::FileLock>(second));

    auto exclusive = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    CHECK(std::holds_alternative<imza::FileLockError>(exclusive));

    first = std::variant<imza::FileLock, imza::FileLockError> {
        imza::FileLockError { }
    };
    second = std::variant<imza::FileLock, imza::FileLockError> {
        imza::FileLockError { }
    };
    exclusive = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    CHECK(std::holds_alternative<imza::FileLock>(exclusive));

    auto shared = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::SHARED, false });
    CHECK(std::holds_alternative<imza::FileLockError>(shared));
#endif
}

TEST_CASE("exclusive holder blocks a shared request")
{
#ifdef _WIN32
    return;
#else
    const auto path = temp_path("excl.lock");
    auto first      = imza::acquire_file_lock(path);
    REQUIRE(std::holds_alternative<imza::FileLock>(first));

    auto shared = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::SHARED, false });
    CHECK(std::holds_alternative<imza::FileLockError>(shared));

    // A shared release must not unlink the file an exclusive holder owns.
    shared = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::SHARED, false });
    CHECK(std::holds_alternative<imza::FileLockError>(shared));
    CHECK(std::filesystem::exists(path));
#endif
}
