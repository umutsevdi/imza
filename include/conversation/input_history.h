#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "common/types.h"

namespace imza {

class InputHistoryStore final : public ApplicationComponent {
public:
    explicit InputHistoryStore(
        std::filesystem::path path, std::size_t limit = 100);

    std::vector<std::string> entries() const;
    Status record(std::string text);

private:
    std::filesystem::path _path;
    std::size_t _limit;
    mutable std::mutex _mutex;
    std::vector<std::string> _entries;
};

} // namespace imza
