#include "Order.h"

#include <algorithm>
#include <vector>

namespace voc
{

std::string_view idOf (ModuleId m) noexcept { return kModuleIds[(size_t) m]; }

int moduleIndexOf (std::string_view id) noexcept
{
    for (size_t k = 0; k < kModuleIds.size(); ++k)
        if (kModuleIds[k] == id) return (int) k;
    return -1;
}

int Order::slotOf (ModuleId m) const noexcept
{
    for (int i = 0; i < kNumModules; ++i)
        if (slots[(size_t) i] == m) return i;
    return -1;
}

bool Order::valid() const noexcept
{
    std::array<bool, kNumModules> seen {};
    for (auto m : slots)
    {
        const auto k = (size_t) m;
        if (k >= (size_t) kNumModules || seen[k]) return false;
        seen[k] = true;
    }
    return true;
}

Order Order::moved (int from, int to) const noexcept
{
    Order o = *this;
    from = std::clamp (from, 0, kNumModules - 1);
    to = std::clamp (to, 0, kNumModules - 1);
    if (from == to) return o;
    const auto m = o.slots[(size_t) from];
    if (from < to)
        std::copy (o.slots.begin() + from + 1, o.slots.begin() + to + 1, o.slots.begin() + from);
    else
        std::copy_backward (o.slots.begin() + to, o.slots.begin() + from, o.slots.begin() + from + 1);
    o.slots[(size_t) to] = m;
    return o;
}

uint64_t Order::pack() const noexcept
{
    uint64_t w = ~uint64_t (0);   // empty slots read 0xF
    for (int i = 0; i < kNumModules; ++i)
    {
        w &= ~(uint64_t (0xF) << (4 * i));
        w |= uint64_t ((uint8_t) slots[(size_t) i] & 0xF) << (4 * i);
    }
    return w;
}

Order Order::unpack (uint64_t packed) noexcept
{
    Order o;
    for (int i = 0; i < kNumModules; ++i)
        o.slots[(size_t) i] = (ModuleId) ((packed >> (4 * i)) & 0xF);
    return o.valid() ? o : Order();
}

std::string Order::toString() const
{
    std::string s;
    for (int i = 0; i < kNumModules; ++i)
    {
        if (i > 0) s += ',';
        s += idOf (slots[(size_t) i]);
    }
    return s;
}

Order Order::parse (std::string_view text)
{
    std::vector<ModuleId> list;
    std::array<bool, kNumModules> present {};
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = std::min (text.find (',', start), text.size());
        auto token = text.substr (start, end - start);
        while (! token.empty() && token.front() == ' ') token.remove_prefix (1);
        while (! token.empty() && token.back() == ' ') token.remove_suffix (1);
        const int k = moduleIndexOf (token);
        if (k >= 0 && ! present[(size_t) k])
        {
            present[(size_t) k] = true;
            list.push_back ((ModuleId) k);
        }
        start = end + 1;
    }

    // Missing modules, in default order: after the nearest present module
    // that precedes them by default.
    for (int d = 0; d < kNumModules; ++d)
    {
        const auto m = kDefaultOrder[(size_t) d];
        if (present[(size_t) m]) continue;
        auto at = list.begin();
        for (int p = d - 1; p >= 0; --p)
        {
            const auto it = std::find (list.begin(), list.end(), kDefaultOrder[(size_t) p]);
            if (it != list.end()) { at = it + 1; break; }
        }
        list.insert (at, m);
        present[(size_t) m] = true;
    }

    Order o;
    std::copy (list.begin(), list.end(), o.slots.begin());
    return o;
}

int firstDifference (const Order& a, const Order& b) noexcept
{
    for (int i = 0; i < kNumModules; ++i)
        if (a[i] != b[i]) return i;
    return kNumModules;
}

} // namespace voc
