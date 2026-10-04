#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hybridclr
{
namespace dhe
{
    // Names belong to the cache, so equal literals, temporary strings and
    // reused caller buffers have the same semantics. No allocation on a hit.
    template<class Value, size_t Capacity = 64>
    class AotGuardCache
    {
        static_assert(Capacity && !(Capacity & (Capacity - 1)), "Cache capacity must be a power of two");
        struct Entry
        {
            const void* epoch = nullptr;
            uint32_t token = 0;
            std::string name;
            Value value{};
        };
        std::array<Entry, Capacity> _entries{};
        static size_t Index(uint32_t token) { return (token ^ (token >> 16)) & (Capacity - 1); }
    public:
        bool TryGet(const void* epoch, const char* name, uint32_t token, Value& value) const
        {
            const Entry& entry = _entries[Index(token)];
            if (entry.epoch != epoch || entry.token != token || entry.name != name) return false;
            value = entry.value;
            return true;
        }
        void Put(const void* epoch, const char* name, uint32_t token, Value value)
        {
            Entry& entry = _entries[Index(token)];
            entry.name = name;
            entry.token = token;
            entry.value = value;
            entry.epoch = epoch;
        }
    };
}
}
