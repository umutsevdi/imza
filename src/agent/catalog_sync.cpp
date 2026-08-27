#include "controller.h"

#include "pricing.h"

#include <future>
#include <memory>
#include <utility>

namespace ursa {

void Controller::ensure_catalog_fresh()
{
    std::lock_guard lock(data_mutex_);
    if (catalog_syncing_ || !catalog_stale(catalog_)) {
        return;
    }
    catalog_syncing_ = true;
    auto future      = std::async(std::launch::async, [] {
        Catalog catalog;
        const Status st = fetch_catalog(catalog);
        return std::make_pair(st, catalog);
    }).share();
    catalog_waiter_.emplace([this, future] {
        future.wait();
        if (!alive_.load()) {
            return;
        }
        _post([this, future] {
            auto [st, catalog] = future.get();
            {
                std::lock_guard lock(data_mutex_);
                catalog_syncing_ = false;
                if (st != Status::OK) {
                    return;
                }
                catalog_ = catalog;
                save_catalog(presets_path(), catalog_);
                set_pricing_catalog(catalog_);
            }
            session_->bump_modal_serial();
        });
    });
}

} // namespace ursa