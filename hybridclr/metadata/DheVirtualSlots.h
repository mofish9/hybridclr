#pragma once

#include "../Il2CppCompatibleDef.h"

namespace hybridclr
{
namespace metadata
{
    // An AOT abstract class can leave its physical vtable entry empty even
    // though its method table contains the virtual declaration for that slot.
    // The caller initializes metadata and normalizes missing-entry stubs first.
    inline const MethodInfo* FindDheVirtualSlotDeclaration(const MethodInfo* vtableMethod,
        const MethodInfo* const* methods, uint16_t methodCount, uint16_t slot)
    {
        if (vtableMethod)
            return vtableMethod;
        for (uint16_t index = 0; index < methodCount; ++index)
        {
            const MethodInfo* method = methods[index];
            if (method && method->slot == slot && (method->flags & METHOD_ATTRIBUTE_VIRTUAL))
                return method;
        }
        return nullptr;
    }
}
}
