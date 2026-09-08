#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

struct MethodInfo;

namespace hybridclr
{
namespace metadata
{
    // Base slots never move. Current-only methods occupy an append-only suffix.
    inline bool BindDheInterfaceSlot(std::vector<const MethodInfo*>& slots,
        uint16_t baseSlot, const MethodInfo* current, uint16_t& logicalSlot)
    {
        if (!current || std::find(slots.begin(), slots.end(), current) != slots.end())
            return false;
        if (baseSlot == UINT16_MAX)
        {
            if (slots.size() >= UINT16_MAX)
                return false;
            logicalSlot = static_cast<uint16_t>(slots.size());
            slots.push_back(current);
            return true;
        }
        if (baseSlot >= slots.size() || slots[baseSlot])
            return false;
        logicalSlot = baseSlot;
        slots[baseSlot] = current;
        return true;
    }
}
}
