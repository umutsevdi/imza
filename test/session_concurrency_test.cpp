#include <atomic>
#include <chrono>
#include <thread>

#include <doctest/doctest.h>

#include "common/tool_call.h"
#include "conversation/session.h"

namespace {

imza::TodoList make_todo(std::size_t round)
{
    imza::TodoList list;
    for (std::size_t i = 0; i < 4; ++i) {
        list.items.push_back(imza::TodoItem {
            "task " + std::to_string(round) + "-" + std::to_string(i),
            imza::TodoItem::Status::PENDING });
    }
    return list;
}

} // namespace

// The crash this guards: a worker replacing session state while the UI reads
// a snapshot. A dangling view would fault in the reader loop.
TEST_CASE("session snapshots stay valid while a worker mutates")
{
    imza::Session session;
    std::atomic<bool> stop { false };
    std::atomic<std::size_t> observed { 0 };

    std::thread writer([&] {
        for (std::size_t round = 0; !stop.load(); ++round) {
            session.set_todo(make_todo(round));
            session.begin_send("prompt " + std::to_string(round));
            session.append_assistant();
        }
    });

    std::thread reader([&] {
        while (!stop.load()) {
            const auto todo   = session.todo();
            const auto items  = session.items();
            std::size_t total = items->size();
            for (const imza::TodoItem& item : todo->items) {
                total += item.content.size();
            }
            observed.store(total);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    stop.store(true);
    writer.join();
    reader.join();
    CHECK(observed.load() > 0);
}

// Copy-on-write: a snapshot keeps its contents when the session mutates.
TEST_CASE("a session snapshot is immutable across later mutations")
{
    imza::Session session;
    session.set_todo(make_todo(0));
    const auto before = session.todo();
    REQUIRE(before->items.size() == 4);

    session.set_todo(make_todo(1));
    session.append_item(imza::UserTurn { "later", { } });

    CHECK(before->items.size() == 4);
    CHECK(before->items.front().content == "task 0-0");
    CHECK(session.todo()->items.front().content == "task 1-0");
}
