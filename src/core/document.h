#pragma once
// FR-26: every edit is a JSON patch applied through Document::apply; undo applies the stored inverse.
#include "core/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sv {

class Document {
  public:
    explicit Document(json data = new_project()) : data_(std::move(data)) {}

    const json &data() const { return data_; }
    // UI state (card positions, camera bookmarks) saved with the project but outside the undo history.
    json &layout() { return data_["layout"]; }

    // Applies an RFC 6902 patch as one undoable step. Throws (leaving the document unchanged) if it does not apply.
    void apply(const json &patch, std::string label);

    bool can_undo() const { return cursor_ > 0; }
    bool can_redo() const { return cursor_ < history_.size(); }
    void undo();
    void redo();

    struct Step {
        std::string label;
        json forward, inverse;
    };
    // Edit history for display; steps before cursor() are done, the rest can be redone.
    const std::vector<Step> &history() const { return history_; }
    std::size_t cursor() const { return cursor_; }

    bool dirty() const { return cursor_ != saved_cursor_; }
    void mark_saved() { saved_cursor_ = cursor_; }
    void mark_unsaved() { saved_cursor_ = SIZE_MAX; } // e.g. recovered from an autosave

  private:
    json data_;
    std::vector<Step> history_; // shortcut: unlimited and kept whole, cap or compact if sessions grow large
    std::size_t cursor_ = 0;
    std::size_t saved_cursor_ = 0;
};

} // namespace sv
