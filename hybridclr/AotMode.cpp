#include "AotModeConfig.h"
#if HYBRIDCLR_ENABLE_AOT_SELECTION
#include "CommonDef.h"
#include <atomic>

namespace hybridclr { namespace startup {
extern const char* const g_deferredAssemblies[];

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

#if HYBRIDCLR_DHE_DIAGNOSTICS
static std::atomic<int> hookProbe{0};
static std::atomic<int> hookCount{0};
void ArmDheImplementations(int poison)
{
    hookCount.store(0, std::memory_order_relaxed);
    hookProbe.store(poison < 0 ? 0 : poison + 1, std::memory_order_release);
}
int CountDheImplementations() { return hookCount.load(std::memory_order_relaxed); }
void ObserveDheImplementation(const char* name)
{
    int armed = hookProbe.load(std::memory_order_acquire);
    if (!armed) return;
    hookCount.fetch_add(1, std::memory_order_relaxed);
    // Disarm before constructing a managed exception: exception allocation
    // itself can traverse the extension boundary.
    if (armed == 2 && hookProbe.exchange(0, std::memory_order_acq_rel) == 2)
        RaiseExecutionEngineException("LAB_DHE_IMPLEMENTATION_FAULT");
}
#endif
}}
#endif
