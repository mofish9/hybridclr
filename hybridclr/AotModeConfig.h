#pragma once
#include "generated/UnityVersion.h"
#ifndef HYBRIDCLR_ENABLE_AOT_SELECTION
#define HYBRIDCLR_ENABLE_AOT_SELECTION 0
#endif

// Only inside a DHE implementation already reached through the selected table.
// Avoid rebinding nested calls while keeping feature-disabled calls unchanged.
#define HCLR_AOT_DIRECT(name) HCLR_AOT_IMPL(name)
#if HYBRIDCLR_ENABLE_AOT_SELECTION
#define HCLR_AOT_IMPL(name) DheImpl_##name
namespace hybridclr { namespace startup {
    // MetadataCache serializes assembly registration and this publication under g_MetadataLock.
    int BindMode(int mode);
    int GetMode();
    bool IsDeferredAssembly(const char* name);
    void RequireDheMode();
    void RequireSelectedMode();
}}
#else
#define HCLR_AOT_IMPL(name) name
#endif
