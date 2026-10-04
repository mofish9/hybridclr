#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace hybridclr
{
namespace dhe
{
    // Keep common call frames on the caller stack. The bounded overflow keeps
    // the existing heap behavior for large value/byref signatures and reentry
    // owns an independent buffer rather than borrowing one TLS scratch array.
    template<class Value, size_t InlineCapacity = 32>
    class InvokeBuffer
    {
        std::array<Value, InlineCapacity> _storage;
        std::vector<Value> _overflow;
        size_t _size;
    public:
        explicit InvokeBuffer(size_t size) : _overflow(size > InlineCapacity ? size : 0), _size(size)
        {
            if (UsesInlineStorage()) std::fill_n(_storage.data(), size, Value{});
        }
        bool UsesInlineStorage() const { return _size <= InlineCapacity; }
        Value* data() { return UsesInlineStorage() ? _storage.data() : _overflow.data(); }
        Value& operator[](size_t index) { return data()[index]; }
    };
}
}
