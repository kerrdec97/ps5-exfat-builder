#pragma once

// Accessible interfaces for the controls this library paints itself. Qt knows
// its own widgets; a self-painted one is a nameless "client area" to a screen
// reader unless it says what it is:
//
//  * Badge        — static text: the status word
//  * ProgressBar  — progress bar with a 0..100 value ("busy" while indeterminate)
//  * Segmented, ChoiceTiles — a choice whose value is the current option
//  * Link         — link
//  * StatTile     — the overline and the value; a button when it is clickable
//  * any widget with the dynamic property "fwRole" = "button": a button; with
//    "fwExpanded" (bool) also expanded / collapsed (the Disclosure header,
//    the header chips)
//
// Installed once by theme::apply().

namespace fw::ui {

void installAccessibility();

} // namespace fw::ui
