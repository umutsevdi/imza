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

ftxui::Element subscription_signin_element(const SubscriptionSigninView& view)
{
    using namespace ftxui;
    Elements rows;
    if (!view.code.empty()) {
        rows.push_back(
            hbox({ text("Device code  ") | bold, text(view.code) | bold }));
    }
    if (!view.url.empty()) {
        // Shown in full so a remote (SSH) session can open it by hand; the
        // hyperlink decorator makes it clickable where the terminal supports
        // OSC 8.
        rows.push_back(hbox({ text("Verification ") | bold,
            text(view.url) | color(HL_CYAN) | underlined
                | hyperlink(view.url) }));
    }
    switch (view.phase) {
    case SubscriptionSigninView::Phase::STARTING:
        rows.push_back(text("requesting sign-in code…") | dim);
        break;
    case SubscriptionSigninView::Phase::WAITING:
        rows.push_back(text("waiting for browser authorization…") | dim);
        break;
    case SubscriptionSigninView::Phase::FAILED:
        rows.push_back(text(view.error) | color(HL_RED));
        break;
    case SubscriptionSigninView::Phase::IDLE: break;
    }
    return vbox(std::move(rows));
}

namespace {

    using namespace ftxui;

    using Phase = SubscriptionSigninView::Phase;

    struct SigninData {
        std::atomic<bool> active { true };
        SubscriptionSigninView view;
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
            rows.push_back(subscription_signin_element(_data->view));
            rows.push_back(_button->Render());
            return vbox(std::move(rows));
        }

    private:
        void _sync_button()
        {
            switch (_data->view.phase) {
            case Phase::WAITING: _button_label = "Open browser"; return;
            case Phase::FAILED: _button_label = "Retry"; return;
            case Phase::IDLE:
            case Phase::STARTING: _button_label = "Connect"; return;
            }
        }

        void _primary()
        {
            switch (_data->view.phase) {
            case Phase::WAITING:
                if (!_data->view.url.empty()) {
                    open_browser(_data->view.url);
                }
                return;
            case Phase::STARTING: return;
            case Phase::IDLE:
            case Phase::FAILED: _start(); return;
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
                _data->view.phase = Phase::FAILED;
                _data->view.error = "Set a label above first - this provider "
                                    "is already connected.";
                return;
            }
            _data->view       = SubscriptionSigninView { };
            _data->view.phase = Phase::STARTING;
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
                    data->view.phase = Phase::FAILED;
                    data->view.error = result.error;
                    return;
                }
                const std::string label = label_getter ? label_getter() : "";
                const std::string key   = connection_key_for(id, label);
                const Config config     = state->providers->config();
                const bool exists
                    = find_connection(config.providers, key) != nullptr;
                if (exists) {
                    data->view.phase = Phase::FAILED;
                    data->view.error
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
                    current->view.phase = Phase::WAITING;
                    current->view.code  = code.user_code;
                    current->view.url   = code.verification_url;
                    open_browser(current->view.url);
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
