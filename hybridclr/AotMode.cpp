#include "AotModeConfig.h"
#if HYBRIDCLR_ENABLE_AOT_SELECTION
#include "CommonDef.h"
#include "metadata/MetadataModule.h"

namespace hybridclr { namespace startup {
extern const char* const g_deferredAssemblies[];

// The pre-DHE field token lookup, retained independently of Current overlays.
const FieldInfo* TraditionalFieldReference(const Il2CppType& type, const Il2CppFieldDefinition* fieldDef)
{
    Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(&type);
    const char* name = il2cpp::vm::GlobalMetadata::GetStringFromIndex(fieldDef->nameIndex);
    void* iter = nullptr;
    while (const FieldInfo* field = il2cpp::vm::Class::GetFields(klass, &iter))
        if (field->token == fieldDef->token)
        {
            IL2CPP_ASSERT(std::strcmp(field->name, name) == 0);
            return field;
        }
    RaiseMissingFieldException(&type, name);
    return nullptr;
}

bool IsDeferredAssembly(const char* name)
{
    for (const char* const* entry = g_deferredAssemblies; *entry; ++entry)
        if (std::strcmp(*entry, name) == 0) return true;
    return false;
}

void RequireSelectedMode()
{
    if (!GetMode())
        il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetInvalidOperationException(
            "Select HybridCLR execution mode from ordinary AOT before loading assemblies."));
}

void RequireDheMode()
{
    if (GetMode() != 1)
        il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetInvalidOperationException(
            "DHE loading requires the process to have selected DHE execution mode."));
}


}}
#endif
