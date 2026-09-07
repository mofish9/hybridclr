#pragma once

#include "AOTHomologousImage.h"

namespace hybridclr
{
namespace metadata
{
    // Physical Base slots stay stable. Logical properties occupy a separate
    // suffix because their PropertyInfo objects are not in klass->properties.
    inline bool TryGetDheAttributePropertyIndex(AOTHomologousImage* image,
        const PropertyInfo* property, int32_t& index)
    {
        Il2CppClass* klass = property->parent;
        if (!image || !image->HasLogicalPropertyView(klass))
            return false;
        void* iter = nullptr;
        const PropertyInfo* candidate = image->GetFirstLogicalProperty(klass, &iter);
        int32_t candidateIndex = klass->property_count;
        while (candidate)
        {
            if (candidate == property)
            {
                index = candidateIndex;
                return true;
            }
            ++candidateIndex;
            if (!image->TryGetNextLogicalProperty(klass, &iter, &candidate))
                break;
        }
        return false;
    }

    inline const PropertyInfo* GetDheAttributePropertyByIndex(AOTHomologousImage* image,
        Il2CppClass* klass, uint32_t index)
    {
        if (index < klass->property_count)
        {
#if UNITY_ENGINE_TUANJIE
            return klass->properties[index];
#else
            return &klass->properties[index];
#endif
        }
        if (!image || !image->HasLogicalPropertyView(klass))
            return nullptr;
        index -= klass->property_count;
        void* iter = nullptr;
        const PropertyInfo* property = image->GetFirstLogicalProperty(klass, &iter);
        while (property && index != 0)
        {
            --index;
            if (!image->TryGetNextLogicalProperty(klass, &iter, &property))
                return nullptr;
        }
        return property;
    }
}
}
