#include "ui/ui.h"

#include "app/application_state.h"
#include "app/flows.h"
#include "platform/command_runner.h"
#include "providers/catalog.h"
#include "providers/subscriptions.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace imza {

namespace {

    using namespace ftxui;

    struct SigninData {
        enum class Phase { IDLE, STARTING, WAITING, FAILED };
        std::atomic<bool> active { true };
        Phase phase = Phase::IDLE;
        std::string code;
        std::string url;
        std::string error;
    };

    ConnectResult connect_result(std::string id, const std::string& label,
        const SubscriptionCredentials& credentials)
    {
        ConnectResult result;
        result.id            = std::move(id);
        result.label         = label;
        result.api_key       = credentials.access_token;
        result.refresh_token = credentials.refresh_token;
        result.expires_at    = credentials.expires_at;
        result.account_id    = credentials.account_id;
        return result;
    }

    class SubscriptionSignin final : public ComponentBase {
    public:
        SubscriptionSignin(std::shared_ptr<ApplicationState> state,
            std::string id, std::function<std::string()> label = { })
            : state_(std::move(state))
            , id_(std::move(id))
            , label_(std::move(label))
            , data_(std::make_shared<SigninData>())
        {
            ButtonOption option;
            option.label     = &button_label_;
            option.on_click  = [this] { _primary(); };
            option.transform = [](const EntryState& entry) {
                Element element = text(" " + entry.label + " ");
                if (entry.focused) {
                    return element | bold | bgcolor(PANEL_COLOR_FOCUS)
                        | color(PANEL_FG);
                }
                return element | bgcolor(PANEL_BORDER) | color(PANEL_FG);
            };
            button_ = Button(option);
            Add(Container::Vertical({ button_ }));
        }

        ~SubscriptionSignin() override
        {
            data_->active.store(false);
            worker_.reset();
        }

        Element OnRender() override
        {
            _sync_button();
            Elements rows;
            if (!data_->code.empty()) {
                rows.push_back(hbox({ text("Device code  ") | bold,
                    text(data_->code) | bold }));
            }
            rows.push_back(button_->Render());
            if (data_->phase == SigninData::Phase::STARTING) {
                rows.push_back(text("requesting sign-in code…") | dim);
            } else if (data_->phase == SigninData::Phase::WAITING) {
                rows.push_back(
                    text("waiting for browser authorization…") | dim);
            } else if (data_->phase == SigninData::Phase::FAILED) {
                rows.push_back(text(data_->error) | color(HL_RED));
            }
            return vbox(std::move(rows));
        }

    private:
        void _sync_button()
        {
            switch (data_->phase) {
            case SigninData::Phase::WAITING:
                button_label_ = "Open browser";
                return;
            case SigninData::Phase::FAILED: button_label_ = "Retry"; return;
            case SigninData::Phase::IDLE:
            case SigninData::Phase::STARTING: button_label_ = "Connect"; return;
            }
        }

        void _primary()
        {
            switch (data_->phase) {
            case SigninData::Phase::WAITING:
                if (!data_->url.empty()) {
                    open_browser(data_->url);
                }
                return;
            case SigninData::Phase::STARTING: return;
            case SigninData::Phase::IDLE:
            case SigninData::Phase::FAILED: _start(); return;
            }
        }

        bool _label_free()
        {
            const std::string label = label_ ? label_() : "";
            const std::string key   = label.empty() ? id_ : id_ + "/" + label;
            const Config config     = state_->providers->config();
            return !std::any_of(config.providers.begin(),
                config.providers.end(), [&](const Connection& connection) {
                    return connection_key(connection) == key;
                });
        }

        void _start()
        {
            if (!_label_free()) {
                data_->phase = SigninData::Phase::FAILED;
                data_->error = "Set a label above first — this provider is "
                               "already connected.";
                return;
            }
            data_->phase = SigninData::Phase::STARTING;
            data_->code.clear();
            data_->url.clear();
            data_->error.clear();
            worker_.reset();
            start_openai();
        }

        void post_result(SubscriptionResult result)
        {
            const std::weak_ptr<SigninData> weak          = data_;
            const std::shared_ptr<ApplicationState> state = state_;
            const std::string id                          = id_;
            state_->post([weak, state, id, result = std::move(result),
                             label_getter = label_] {
                const auto data = weak.lock();
                if (!data || !data->active.load()) {
                    return;
                }
                if (result.status != Status::OK) {
                    data->phase = SigninData::Phase::FAILED;
                    data->error = result.error;
                    return;
                }
                const std::string label = label_getter ? label_getter() : "";
                const std::string key   = label.empty() ? id : id + "/" + label;
                const Config config     = state->providers->config();
                const bool exists       = std::any_of(config.providers.begin(),
                    config.providers.end(), [&](const Connection& connection) {
                        return connection_key(connection) == key;
                    });
                if (exists) {
                    data->phase = SigninData::Phase::FAILED;
                    data->error
                        = "Already connected — set a different label above.";
                    return;
                }
                imza::resolve_modal(*state,
                    ModalResult {
                        connect_result(id, label, result.credentials) });
            });
        }

        void start_openai()
        {
            const std::weak_ptr<SigninData> weak          = data_;
            const std::shared_ptr<ApplicationState> state = state_;
            worker_.emplace([this, weak, state](std::stop_token stop) {
                const OpenAIDeviceCodeResult requested
                    = request_openai_device_code();
                const auto data = weak.lock();
                if (!data || !data->active.load()) {
                    return;
                }
                if (requested.status != Status::OK) {
                    post_result({ requested.status, { }, requested.error });
                    return;
                }
                state->post([weak, code = requested.code] {
                    const auto current = weak.lock();
                    if (!current || !current->active.load()) {
                        return;
                    }
                    current->phase = SigninData::Phase::WAITING;
                    current->code  = code.user_code;
                    current->url   = code.verification_url;
                    open_browser(current->url);
                });
                post_result(await_openai_device_code(requested.code, stop));
            });
        }

        std::shared_ptr<ApplicationState> state_;
        std::string id_;
        std::function<std::string()> label_;
        std::shared_ptr<SigninData> data_;
        std::optional<std::jthread> worker_;
        std::string button_label_ = "Connect";
        Component button_;
    };

} // namespace

ftxui::Component make_subscription_signin(
    std::shared_ptr<ApplicationState> state, std::string connection_id,
    std::function<std::string()> label)
{
    return ftxui::Make<SubscriptionSignin>(
        std::move(state), std::move(connection_id), std::move(label));
}

} // namespace imza
