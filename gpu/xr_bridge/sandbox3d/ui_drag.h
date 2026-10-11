// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: MIT
//
// Drag and drop in the dashboard (ui.cpp UpdateDashboard): squeeze the grip with the pointer on an
// inventory card (or on an occupied quick slot) to pick it up, it follows the pointer, move it onto a
// quick slot and let go of the grip to assign it. Pure state, no I/O: unit tested in vr_game_ui_test.cpp.
//
//   grid card   -> empty quick slot       Assign
//   grid card   -> occupied quick slot    Replace (the old item stays in the inventory, just out of the slot)
//   quick slot  -> the inventory grid     Unassign (the game's clear: the slot's inventory index -1)
//   anything else (the same slot, off the panel, another quick slot, a button) -> Cancel; nothing changes.
// A card that already sits in a quick slot is picked up AS that slot (Source::Quick): moving an item
// from one quick slot to another is not offered (the game's equip command would leave it in two slots);
// drop it on the grid first, then on the slot it should go to.
#pragma once

#include <string>
#include <vector>

namespace sb {

class UiDrag {
public:
    enum class Source { None, Grid, Quick };
    enum class Result { None, Assign, Replace, Unassign, Cancel };

    struct Drop {
        Result result = Result::None;
        std::string item;      // the dragged item
        int slot = -1;         // Assign / Replace: the target quick slot; Unassign: the slot it left
        std::string replaced;  // Replace: the item that was in the slot
        std::string reason;    // Cancel: why (for the toast), may be empty
    };

    bool Active() const { return active; }
    int Hand() const { return hand; }
    const std::string& Item() const { return item; }
    Source From() const { return source; }
    int SourceIndex() const { return source_index; }
    /// The quick slot the item is over now (-1 none), whether it is over the inventory grid.
    int HoverSlot() const { return hover_slot; }
    bool OverGrid() const { return over_grid; }
    /// What a drop here would do (for the highlight): the same rules as Release.
    Result Preview(const std::vector<std::string>& quick) const { return Decide(quick).result; }

    /// Picks `key` up with `hand`. False when one is already being dragged or the key is empty.
    bool Pick(int h, const std::string& key, Source from, int index) {
        if (active || key.empty() || from == Source::None || (h != 0 && h != 1)) {
            return false;
        }
        active = true;
        hand = h;
        item = key;
        source = from;
        source_index = index;
        hover_slot = -1;
        over_grid = false;
        return true;
    }

    /// Where the pointer is now. Returns true when the target changed (a haptic tick).
    bool Hover(int slot, bool grid) {
        if (!active) {
            return false;
        }
        const bool changed = slot != hover_slot || grid != over_grid;
        hover_slot = slot;
        over_grid = grid;
        return changed;
    }

    /// The grip was let go: what to do. The drag ends either way.
    Drop Release(const std::vector<std::string>& quick) {
        const Drop d = Decide(quick);
        Reset();
        return d;
    }

    /// The drag ends without a drop (the menu closed, the hand lost).
    void Cancel() { Reset(); }

private:
    bool active = false;
    int hand = -1;
    std::string item;
    Source source = Source::None;
    int source_index = -1;
    int hover_slot = -1;
    bool over_grid = false;

    void Reset() {
        active = false;
        hand = -1;
        item.clear();
        source = Source::None;
        source_index = -1;
        hover_slot = -1;
        over_grid = false;
    }

    Drop Decide(const std::vector<std::string>& quick) const {
        Drop d;
        d.item = item;
        if (!active) {
            return d;
        }
        d.result = Result::Cancel;
        if (source == Source::Grid) {
            if (hover_slot < 0 || size_t(hover_slot) >= quick.size()) {
                return d; // dropped elsewhere
            }
            d.slot = hover_slot;
            d.replaced = quick[size_t(hover_slot)];
            if (d.replaced == item) {
                d.replaced.clear();
                d.reason = "already in that slot";
                return d;
            }
            d.result = d.replaced.empty() ? Result::Assign : Result::Replace;
            return d;
        }
        // From a quick slot: back onto the grid clears it.
        if (over_grid && source_index >= 0 && size_t(source_index) < quick.size()) {
            d.result = Result::Unassign;
            d.slot = source_index;
            return d;
        }
        if (hover_slot >= 0 && hover_slot != source_index) {
            d.reason = "drop it on the inventory first, then on the slot it should go to";
        }
        return d;
    }
};

} // namespace sb
