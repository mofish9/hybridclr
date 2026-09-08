#pragma once

#include "../Il2CppCompatibleDef.h"
#include <cstring>

namespace hybridclr
{
namespace metadata
{
    // Raw Current fields and logical Base aliases use different owners. Only
    // search the supplied physical array, never recurse through the merged view.
    inline FieldInfo* FindDhePhysicalField(Il2CppClass* owner, const FieldInfo* identity)
    {
        if (!owner || !owner->fields || !identity || !identity->name)
            return nullptr;
        for (uint16_t index = 0; index < owner->field_count; ++index)
        {
            FieldInfo* field = owner->fields + index;
            if (field->token == identity->token && field->name &&
                std::strcmp(field->name, identity->name) == 0)
                return field;
        }
        return nullptr;
    }
}
}
