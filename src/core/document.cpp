#include "core/document.h"

namespace sv {

void Document::apply(const json &patch, std::string label) {
    json next = data_.patch(patch); // throws before anything changes
    json inverse = json::diff(next, data_);
    data_ = std::move(next);
    history_.resize(cursor_); // a new edit drops the redo branch
    if (saved_cursor_ > cursor_)
        saved_cursor_ = std::size_t(-1); // the saved state is no longer reachable
    history_.push_back({std::move(label), patch, std::move(inverse)});
    ++cursor_;
}

void Document::undo() {
    if (!can_undo())
        return;
    --cursor_;
    data_ = data_.patch(history_[cursor_].inverse);
}

void Document::redo() {
    if (!can_redo())
        return;
    data_ = data_.patch(history_[cursor_].forward);
    ++cursor_;
}

} // namespace sv
