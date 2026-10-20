#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "platform/file_lock.h"
#include "test_fs.h"
#include "test_helpers.h"

namespace {

using imza::test::TempDir;

} // namespace

TEST_CASE("acquired lock blocks a second holder until released")
{
    const TempDir dir;
    const auto path = dir.file("mutex.lock");
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

    imza::test::release_lock(first);
    second.join();
    CHECK(acquired);
}

TEST_CASE("lock file is removed on release and can be reacquired")
{
    const TempDir dir;
    const auto path = dir.file("persist.lock");
    {
        auto lock = imza::acquire_file_lock(path);
        REQUIRE(std::holds_alternative<imza::FileLock>(lock));
    }
#ifdef _WIN32
    // Windows locks a path-derived named mutex and never creates a file.
    CHECK_FALSE(std::filesystem::exists(path));
#else
    CHECK_FALSE(std::filesystem::exists(path));
#endif
    {
        auto lock = imza::acquire_file_lock(path);
        REQUIRE(std::holds_alternative<imza::FileLock>(lock));
    }
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("non-blocking request fails while another holder is live")
{
    const TempDir dir;
    const auto path = dir.file("try.lock");
    auto first      = imza::acquire_file_lock(path);
    REQUIRE(std::holds_alternative<imza::FileLock>(first));

    auto second = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    CHECK(std::holds_alternative<imza::FileLockError>(second));

    CHECK(imza::file_lock_held(path));

    imza::test::release_lock(first);
    CHECK_FALSE(imza::file_lock_held(path));

    auto third = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    CHECK(std::holds_alternative<imza::FileLock>(third));
    CHECK(imza::file_lock_held(path));
}

TEST_CASE("shared locks coexist and yield to an exclusive holder")
{
    const TempDir dir;
    const auto path = dir.file("shared.lock");
    auto first      = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::SHARED, false });
    REQUIRE(std::holds_alternative<imza::FileLock>(first));

    auto second = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::SHARED, false });
#ifdef _WIN32
    // Named mutexes are exclusive only, so a shared request degrades to an
    // exclusive one and the second holder is refused.
    CHECK(std::holds_alternative<imza::FileLockError>(second));
#else
    REQUIRE(std::holds_alternative<imza::FileLock>(second));

    auto blocked = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    CHECK(std::holds_alternative<imza::FileLockError>(blocked));

    imza::test::release_lock(second);
#endif

    imza::test::release_lock(first);
    auto exclusive = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::EXCLUSIVE, false });
    CHECK(std::holds_alternative<imza::FileLock>(exclusive));
}

TEST_CASE("exclusive holder blocks a shared request")
{
    const TempDir dir;
    const auto path = dir.file("excl.lock");
    auto first      = imza::acquire_file_lock(path);
    REQUIRE(std::holds_alternative<imza::FileLock>(first));

    auto shared = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::SHARED, false });
    CHECK(std::holds_alternative<imza::FileLockError>(shared));

    // A shared release must not unlink the file an exclusive holder owns.
    shared = imza::acquire_file_lock(
        path, imza::FileLockRequest { imza::FileLockMode::SHARED, false });
    CHECK(std::holds_alternative<imza::FileLockError>(shared));
#ifndef _WIN32
    // Only the POSIX backend materializes a lock file to keep around.
    CHECK(std::filesystem::exists(path));
#endif
}
