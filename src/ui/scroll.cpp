#include "ui/ui.h"

#include <ftxui/dom/node.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace imza {

namespace {

    int screen_height(std::int64_t height)
    {
        return static_cast<int>(
            std::min<std::int64_t>(std::max<std::int64_t>(0, height),
                std::numeric_limits<int>::max()));
    }

} // namespace

ftxui::Decorator capture_content_height(std::function<void(int)> out)
{
    class Impl : public ftxui::Node {
    public:
        Impl(ftxui::Element child, std::function<void(int)> out)
            : ftxui::Node(ftxui::Elements { std::move(child) })
            , out_(std::move(out))
        {
        }

        void ComputeRequirement() override
        {
            ftxui::Node::ComputeRequirement();
            requirement_ = children_[0]->requirement();
            out_(requirement_.min_y);
        }

        void SetBox(ftxui::Box box) override
        {
            ftxui::Node::SetBox(box);
            children_[0]->SetBox(box);
        }

    private:
        std::function<void(int)> out_;
    };
    return [out = std::move(out)](ftxui::Element child) {
        return std::make_shared<Impl>(std::move(child), out);
    };
}

ftxui::Decorator capture_content_height(int* out)
{
    return capture_content_height([out](int height) { *out = height; });
}

int ScrollView::viewport_lines() const
{
    if (box.y_max < box.y_min) {
        return 0;
    }
    return box.y_max - box.y_min + 1;
}

int ScrollView::max_scroll() const
{
    return std::max(0, content_height - viewport_lines());
}

void ScrollView::scroll_lines(int delta)
{
    scroll = std::clamp(scroll + delta, 0, max_scroll());
}

ftxui::Element scroll_viewport(ftxui::Element content, ScrollView& view)
{
    view.scroll_lines(0);
    return std::move(content) | capture_content_height(&view.content_height)
        | ftxui::vscroll_indicator
        | ftxui::focusPosition(
            0, view.scroll + std::max(0, view.viewport_lines() - 1) / 2)
        | ftxui::yframe | ftxui::yflex | ftxui::reflect(view.box);
}

bool scroll_viewport_event(ScrollView& view, ftxui::Event& event)
{
    if (event == ftxui::Event::ArrowUp) {
        view.scroll_lines(-1);
        return true;
    }
    if (event == ftxui::Event::ArrowDown) {
        view.scroll_lines(1);
        return true;
    }
    if (event == ftxui::Event::PageUp) {
        view.scroll_lines(-std::max(1, view.viewport_lines() - 1));
        return true;
    }
    if (event == ftxui::Event::PageDown) {
        view.scroll_lines(std::max(1, view.viewport_lines() - 1));
        return true;
    }
    if (event == ftxui::Event::Home) {
        view.scroll = 0;
        return true;
    }
    if (event == ftxui::Event::End) {
        view.scroll = view.max_scroll();
        return true;
    }
    if (event.is_mouse()) {
        const ftxui::Mouse& mouse = event.mouse();
        if (mouse.button == ftxui::Mouse::WheelUp) {
            view.scroll_lines(-3);
            return true;
        }
        if (mouse.button == ftxui::Mouse::WheelDown) {
            view.scroll_lines(3);
            return true;
        }
    }
    return false;
}

VirtualListState::VirtualListState(int estimated_height)
    : _estimated_height(std::max(1, estimated_height))
{
}

void VirtualListState::reset(std::size_t count)
{
    _heights.assign(count, _estimated_height);
    _offsets_dirty = true;
}

void VirtualListState::resize(std::size_t count)
{
    if (_heights.size() == count) {
        return;
    }
    _heights.resize(count, _estimated_height);
    _offsets_dirty = true;
}

int VirtualListState::set_height(const std::size_t index, const int height)
{
    if (index >= _heights.size()) {
        return 0;
    }
    const int next  = std::max(1, height);
    const int delta = next - _heights[index];
    if (delta != 0) {
        _heights[index] = next;
        _offsets_dirty  = true;
    }
    return delta;
}

int VirtualListState::total_height() const
{
    _rebuild_offsets();
    return screen_height(_offsets.back());
}

VirtualListWindow VirtualListState::window(
    const int scroll, const int viewport, const int overscan) const
{
    _rebuild_offsets();
    if (_heights.empty()) {
        return { };
    }
    const std::int64_t first_line
        = std::max<std::int64_t>(0, scroll - std::max(0, overscan));
    const std::int64_t last_line = std::min<std::int64_t>(_offsets.back(),
        static_cast<std::int64_t>(std::max(0, scroll)) + std::max(1, viewport)
            + std::max(0, overscan));
    const auto begin_offset
        = std::upper_bound(_offsets.begin(), _offsets.end(), first_line);
    const std::size_t begin = static_cast<std::size_t>(
        std::max<std::ptrdiff_t>(0, begin_offset - _offsets.begin() - 1));
    const auto end_offset
        = std::lower_bound(_offsets.begin(), _offsets.end(), last_line);
    const std::size_t end = std::clamp<std::size_t>(
        static_cast<std::size_t>(end_offset - _offsets.begin()), begin + 1,
        _heights.size());
    return { begin, end, screen_height(_offsets[begin]),
        screen_height(_offsets.back() - _offsets[end]) };
}

void VirtualListState::_rebuild_offsets() const
{
    if (!_offsets_dirty) {
        return;
    }
    _offsets.assign(_heights.size() + 1, 0);
    for (std::size_t index = 0; index < _heights.size(); ++index) {
        _offsets[index + 1] = _offsets[index] + _heights[index];
    }
    _offsets_dirty = false;
}

} // namespace imza
