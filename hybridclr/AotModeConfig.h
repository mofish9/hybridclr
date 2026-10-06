#pragma once
#include "generated/UnityVersion.h"
#ifndef HYBRIDCLR_ENABLE_AOT_SELECTION
#define HYBRIDCLR_ENABLE_AOT_SELECTION 0
#endif
#if HYBRIDCLR_ENABLE_AOT_SELECTION
#define HCLR_AOT_IMPL(name) DheImpl_##name
namespace hybridclr { namespace startup {
    // MetadataCache serializes assembly registration and this publication under g_MetadataLock.
    int BindMode(int mode);
    int GetMode();
    bool IsDeferredAssembly(const char* name);
    void RequireDheMode();
    void RequireSelectedMode();
    void ObserveDheImplementation(const char* name);
    void ArmDheImplementations(int poison);
    int CountDheImplementations();
}}
#if HYBRIDCLR_DHE_DIAGNOSTICS
#define HCLR_AOT_OBSERVE(name) hybridclr::startup::ObserveDheImplementation(name)
#else
#define HCLR_AOT_OBSERVE(name) ((void)0)
#endif
#else
#define HCLR_AOT_IMPL(name) name
#define HCLR_AOT_OBSERVE(name) ((void)0)
#endif
