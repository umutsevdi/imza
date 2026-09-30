#include "ui/ui.h"

#include "app/application_state.h"
#include "app/flows.h"
#include "platform/command_runner.h"
#include "providers/catalog.h"
#include "providers/subscriptions.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <atomic>
#include <functional>
#include <memory>
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
            : _state(std::move(state))
            , _id(std::move(id))
            , _label(std::move(label))
            , _data(std::make_shared<SigninData>())
            , _button(action_button(&_button_label, [this] { _primary(); }))
        {
            Add(Container::Vertical({ _button }));
        }

        ~SubscriptionSignin() override
        {
            _data->active.store(false);
            _worker.reset();
        }

        Element OnRender() override
        {
            _sync_button();
            Elements rows;
            if (!_data->code.empty()) {
                rows.push_back(hbox({ text("Device code  ") | bold,
                    text(_data->code) | bold }));
            }
            rows.push_back(_button->Render());
            if (_data->phase == SigninData::Phase::STARTING) {
                rows.push_back(text("requesting sign-in code…") | dim);
            } else if (_data->phase == SigninData::Phase::WAITING) {
                rows.push_back(
                    text("waiting for browser authorization…") | dim);
            } else if (_data->phase == SigninData::Phase::FAILED) {
                rows.push_back(text(_data->error) | color(HL_RED));
            }
            return vbox(std::move(rows));
        }

    private:
        void _sync_button()
        {
            switch (_data->phase) {
            case SigninData::Phase::WAITING:
                _button_label = "Open browser";
                return;
            case SigninData::Phase::FAILED: _button_label = "Retry"; return;
            case SigninData::Phase::IDLE:
            case SigninData::Phase::STARTING: _button_label = "Connect"; return;
            }
        }

        void _primary()
        {
            switch (_data->phase) {
            case SigninData::Phase::WAITING:
                if (!_data->url.empty()) {
                    open_browser(_data->url);
                }
                return;
            case SigninData::Phase::STARTING: return;
            case SigninData::Phase::IDLE:
            case SigninData::Phase::FAILED: _start(); return;
            }
        }

        bool _label_free()
        {
            const std::string label = _label ? _label() : "";
            const std::string key   = connection_key_for(_id, label);
            const Config config     = _state->providers->config();
            return find_connection(config.providers, key) == nullptr;
        }

        void _start()
        {
            if (!_label_free()) {
                _data->phase = SigninData::Phase::FAILED;
                _data->error = "Set a label above first - this provider is "
                               "already connected.";
                return;
            }
            _data->phase = SigninData::Phase::STARTING;
            _data->code.clear();
            _data->url.clear();
            _data->error.clear();
            _worker.reset();
            _start_openai();
        }

        void _post_result(SubscriptionResult result)
        {
            const std::weak_ptr<SigninData> weak          = _data;
            const std::shared_ptr<ApplicationState> state = _state;
            const std::string id                          = _id;
            _state->post([weak, state, id, result = std::move(result),
                             label_getter = _label] {
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
                const std::string key   = connection_key_for(id, label);
                const Config config     = state->providers->config();
                const bool exists
                    = find_connection(config.providers, key) != nullptr;
                if (exists) {
                    data->phase = SigninData::Phase::FAILED;
                    data->error
                        = "Already connected - set a different label above.";
                    return;
                }
                imza::resolve_modal(*state,
                    ModalResult {
                        connect_result(id, label, result.credentials) });
            });
        }

        void _start_openai()
        {
            const std::weak_ptr<SigninData> weak          = _data;
            const std::shared_ptr<ApplicationState> state = _state;
            _worker.emplace([this, weak, state](std::stop_token stop) {
                const OpenAIDeviceCodeResult requested
                    = request_openai_device_code();
                const auto data = weak.lock();
                if (!data || !data->active.load()) {
                    return;
                }
                if (requested.status != Status::OK) {
                    _post_result({ requested.status, { }, requested.error });
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
                _post_result(await_openai_device_code(requested.code, stop));
            });
        }

        std::shared_ptr<ApplicationState> _state;
        std::string _id;
        std::function<std::string()> _label;
        std::shared_ptr<SigninData> _data;
        std::optional<std::jthread> _worker;
        std::string _button_label = "Connect";
        Component _button;
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
