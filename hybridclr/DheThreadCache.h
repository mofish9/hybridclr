#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hybridclr
{
namespace dhe
{
    // Each instance belongs to one thread. Values are copied from immutable
    // process-lifetime metadata; no mutable shared container is read here.
    // Publication identity is part of every key, including cached misses.
    template<class Value, class Epoch = const void*, size_t Capacity = 64>
    class ThreadCache
    {
        static_assert(Capacity && !(Capacity & (Capacity - 1)), "Cache capacity must be a power of two");
        struct Entry
        {
            Epoch epoch{};
            const void* owner = nullptr;
            const void* context = nullptr;
            uintptr_t slot = 0;
            Value value{};
        };
        std::array<Entry, Capacity> _entries{};

        static size_t Index(const void* owner, const void* context, uintptr_t slot)
        {
            uintptr_t key = (reinterpret_cast<uintptr_t>(owner) >> 4) ^
                (reinterpret_cast<uintptr_t>(context) >> 4) ^ slot;
            key ^= key >> 11;
            return static_cast<size_t>(key) & (Capacity - 1);
        }

    public:
        bool TryGet(Epoch epoch, const void* owner, const void* context,
            uintptr_t slot, Value& value) const
        {
            const Entry& entry = _entries[Index(owner, context, slot)];
            if (!owner || entry.epoch != epoch || entry.owner != owner ||
                entry.context != context || entry.slot != slot)
                return false;
            value = entry.value;
            return true;
        }

        void Put(Epoch epoch, const void* owner, const void* context,
            uintptr_t slot, Value value)
        {
            Entry& entry = _entries[Index(owner, context, slot)];
            entry.epoch = epoch;
            entry.owner = owner;
            entry.context = context;
            entry.slot = slot;
            entry.value = value;
        }
    };
}
}
