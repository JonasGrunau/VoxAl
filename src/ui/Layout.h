#pragma once

// VoxAl's own layout tokens (the product part; the house grid is in
// Theme.h). Every size in the chain strip, the detail rows and the masthead
// bar comes from here or from theme::sp, so a change moves everything that
// shares it.
namespace layout
{
    // Dials: one ring size everywhere (chain cards and detail rows).
    constexpr float kDisc = 54.f;

    // Section B: twelve cards across the content width, 8 px apart.
    constexpr int kCards = 12;
    constexpr float kCardW = 62.f, kCardGap = 8.f;
    static_assert (kCards * kCardW + (kCards - 1) * kCardGap == 832.f, "the cards fill the content width");
    constexpr float kCardPad = 6.f;                                   // inside a card, all round
    constexpr float kCardHeader = 13.f;                               // power box and name
    constexpr float kCardDialY = kCardPad + kCardHeader + 4.f;        // the dial under the header
    constexpr float kCardValue = 18.f;                                // a compact dial's value under its disc
    constexpr float kCardStatus = 12.f;                               // LEARNING, or what the module does now
    constexpr float kCardH = kCardDialY + kDisc + kCardValue + kCardStatus + kCardPad;   // 113

    // Section C: dial cells and a column of rows (selectors, switches, tags, links).
    constexpr float kCellW = 62.f;
    constexpr float kDialH = kDisc + 4.f + 13.f + 16.f;               // ring, gap, name, value: 87
    constexpr float kRowPitch = 22.f;                                 // one control row
    constexpr float kCtrlH = 17.f;                                    // a selector, switch or tag inside it
    constexpr float kMeterH = 26.f;                                   // a meter or a readout
    constexpr float kMeterW = 150.f;
    constexpr float kColumnGap = 20.f;                                // between the blocks of a detail row (= the group gap)

    // Masthead: keys in the preset bar and the undo/redo pair.
    constexpr float kKeyH = 18.f;
}
