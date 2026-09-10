#pragma once

#include "vm/Class.h"

namespace hybridclr
{
namespace transform
{
    // Frozen execution methods can belong to an interpreter copy of the generic
    // definition. Namespace/name alone does not give that copy Nullable's engine
    // layout contract; its castClass may still be itself. Use its IL body then.
    inline bool CanUseNullableIntrinsic(const Il2CppClass* klass)
    {
        return klass && il2cpp::vm::Class::IsNullable(klass) &&
            klass->castClass && klass->castClass != klass;
    }
}
}
