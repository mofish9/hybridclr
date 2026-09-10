#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

#define HYBRIDCLR_DHE_HAS_CURRENT_EXECUTION 1
#define HYBRIDCLR_DHE_HAS_CURRENT_IMAGE_PLAN 1
#define HYBRIDCLR_DHE_HAS_FROZEN_AOT_SOURCE 1
#define HYBRIDCLR_DHE_HAS_FROZEN_GENERIC_CONTEXT 1
#define HYBRIDCLR_DHE_HAS_INTERPRETER_BATCH 1
#define HYBRIDCLR_DHE_HAS_MODULE_INITIALIZATION 1
#define HYBRIDCLR_DHE_HAS_MODULE_TOKEN_RESOLUTION 1
#define HYBRIDCLR_DHE_HAS_LENGTH_PRESERVED_CONSTANT_STRINGS 1
#define HYBRIDCLR_DHE_HAS_TRACKED_LOAD_PHASE 1

struct Il2CppAssembly;
struct Il2CppClass;
struct MethodInfo;
struct VirtualInvokeData;

namespace hybridclr
{
namespace dhe
{
    // The Base Player embeds one immutable MetaVersion per DHE assembly. At
    // runtime it is compared with the current MetaVersion shipped beside the
    // current managed assembly.
    constexpr uint32_t kMetaVersionSchema = 1;
    constexpr uint32_t kMetaVersionStrictCompatibilityFlag = 1u;
    constexpr uint32_t kMetaVersionKnownFlags = kMetaVersionStrictCompatibilityFlag;
    constexpr uint32_t kMetaVersionKnownTypeFlags = 1u;
    constexpr uint32_t kMetaVersionKnownMethodFlags = 1u | 2u | 4u | 8u | 16u | 32u;
    constexpr size_t kSha256DigestSize = 32;

    using Sha256Digest = std::array<uint8_t, kSha256DigestSize>;

    struct MetaVersionType
    {
        Sha256Digest stableId{};
        Sha256Digest version{};
        uint32_t token = 0;
        uint32_t flags = 0;
    };

    struct MetaVersionMethod
    {
        Sha256Digest stableId{};
        Sha256Digest version{};
        Sha256Digest declaringTypeStableId{};
        uint32_t token = 0;
        uint32_t flags = 0;
    };

    struct MetaVersionData
    {
        std::string assemblyName;
        uint32_t flags = 0;
        Sha256Digest assemblyHash{};
        std::vector<MetaVersionType> types;
        std::vector<MetaVersionMethod> methods;
    };

    struct CurrentMethodExecution
    {
        CurrentMethodExecution() = default;
        CurrentMethodExecution(uint32_t token, const MethodInfo* method)
            : baseMethodToken(token), currentMethod(method) {}
        uint32_t baseMethodToken = 0;
        const MethodInfo* currentMethod = nullptr;
    };

    struct CurrentMetadataTokenBinding
    {
        CurrentMetadataTokenBinding(uint32_t baseValue, uint32_t currentValue)
            : baseToken(baseValue), currentToken(currentValue) {}
        uint32_t baseToken;
        uint32_t currentToken;
        bool operator==(const CurrentMetadataTokenBinding& other) const
        {
            return baseToken == other.baseToken && currentToken == other.currentToken;
        }
    };

    enum class CurrentImageSourceKind : uint8_t
    {
        MutableHotfix = 0,
        FrozenBaseAot = 1,
    };

    struct CurrentImageSource
    {
        CurrentImageSourceKind kind = CurrentImageSourceKind::MutableHotfix;
        // Supplied from the authenticated Base snapshot, never a Current DLL.
        Sha256Digest baseSourceHash{};
        // Sorted Base tokens, including the generated identity type. Its
        // archived initializer is deliberately normalized by the Base workflow.
        std::vector<uint32_t> excludedBaseTypeTokens;
        // Frozen methods selected solely for their concrete generic arguments.
        // An unaffected closed context must keep its Base AOT entry.
        std::vector<uint32_t> genericContextMethodTokens;
        bool operator==(const CurrentImageSource& other) const
        {
            return kind == other.kind && baseSourceHash == other.baseSourceHash &&
                excludedBaseTypeTokens == other.excludedBaseTypeTokens &&
                genericContextMethodTokens == other.genericContextMethodTokens;
        }
    };

    bool ValidateCurrentImageSource(const CurrentImageSource& source,
        const Sha256Digest& baseHash, const Sha256Digest& currentHash);

    // Internal image preparation input. It is bound to one Base/Current pair;
    // it does not change the immutable MV format or authorize a resource load.
    struct CurrentImagePlan
    {
        std::string assemblyName;
        Sha256Digest baseAssemblyHash{};
        Sha256Digest currentAssemblyHash{};
        CurrentImageSource source;
        std::vector<CurrentMetadataTokenBinding> types;
        std::vector<CurrentMetadataTokenBinding> methods;
        bool operator==(const CurrentImagePlan& other) const
        {
            return assemblyName == other.assemblyName && baseAssemblyHash == other.baseAssemblyHash &&
                currentAssemblyHash == other.currentAssemblyHash && source == other.source &&
                types == other.types && methods == other.methods;
        }
    };

    // Select existing declarations by Current tokens, matching their stable
    // identities against Base. Members of selected storage types are included
    // automatically. Additional methods cover callers whose IL is unchanged.
    // A failed selection leaves result untouched. Dependency closure and native
    // boundary checks are obligations of the complete resource preparation.
    bool BuildCurrentImagePlan(const MetaVersionData& baseMetaVersion,
        const MetaVersionData& currentMetaVersion,
        const std::vector<uint32_t>& currentTypeTokens,
        const std::vector<uint32_t>& currentMethodTokens, CurrentImagePlan& result,
        const CurrentImageSource& source = CurrentImageSource{});

    struct MetaVersionRegistration
    {
        MetaVersionRegistration() = default;
        MetaVersionRegistration(const Il2CppAssembly* baseAssemblyValue,
            const MetaVersionData* baseMetaVersionValue,
            const MetaVersionData* currentMetaVersionValue)
            : baseAssembly(baseAssemblyValue),
              baseMetaVersion(baseMetaVersionValue),
              currentMetaVersion(currentMetaVersionValue)
        {
        }

        const Il2CppAssembly* baseAssembly = nullptr;
        const MetaVersionData* baseMetaVersion = nullptr;
        const MetaVersionData* currentMetaVersion = nullptr;
        CurrentImageSource source;
        // Internal preparation result, not a mutable/payload-owned lookup.
        // Registration copies the bindings into its atomic published state.
        // Current metadata may use a new physical value layout even when the
        // immutable Base method fingerprint is unchanged.
        std::vector<CurrentMethodExecution> currentExecutions;
    };

    bool ParseMetaVersion(const void* data, uint32_t size, MetaVersionData& result);
    bool ComputeSha256(const void* data, uint32_t size, Sha256Digest& result);

    bool PrepareChangedMethods(const Il2CppAssembly* assembly,
        const std::vector<uint32_t>& changedMethodTokens,
        std::vector<const MethodInfo*>& resolvedMethods);
    // SUPERSET metadata keeps a current-image MethodInfo for reflection and a
    // logical Base MethodInfo for execution. Register that relationship while
    // the image is built so DHE dispatch can identify methods without relying
    // on current metadata tokens (which may be reordered).
    bool RegisterLogicalMethodMapping(const Il2CppAssembly* assembly,
        const MethodInfo* currentMethod, const MethodInfo* baseMethod);
    bool PrepareAndRegisterMetaVersion(const Il2CppAssembly* baseAssembly,
        const MetaVersionData& baseMetaVersion, const MetaVersionData& currentMetaVersion);
    // Resolve and prepare every assembly first, then publish the complete set
    // with one release store. A failure restores all MethodInfo/vtable state.
    bool PrepareAndRegisterMetaVersions(
        const std::vector<MetaVersionRegistration>& registrations,
        const std::vector<Il2CppAssembly*>& interpreterAssemblies = {});

    bool IsDheAssembly(const Il2CppAssembly* assembly);
    bool IsMutableDheAssembly(const Il2CppAssembly* assembly);
    // Only generated guards for configured hotfix module cctors call this.
    // Unity's eager startup call returns until Current metadata is committed.
    bool IsDheModuleInitializationReady(const char* assemblyName);
    bool IsFrozenAotExecutionSource(const Il2CppAssembly* assembly);
    bool TryGetVirtualInvokeData(const Il2CppClass* klass, uint16_t logicalSlot,
        const VirtualInvokeData*& result);
    // Current aliases can reuse a Base slot number for a different declaration.
    // Calls with method metadata must preserve that identity through dispatch.
    bool TryGetVirtualInvokeData(const Il2CppClass* klass, const MethodInfo* method,
        const VirtualInvokeData*& result);
    bool TryGetVirtualBaseMethod(const MethodInfo* method, bool definition,
        const MethodInfo*& result);
    bool TryGetVirtualReflectionIdentity(const Il2CppClass* reflectedType, const MethodInfo* method,
        const MethodInfo*& result);

    bool TryGetInterfaceInvokeData(const Il2CppClass* klass, const Il2CppClass* interfaceType,
        uint16_t logicalSlot, const VirtualInvokeData*& result);
    bool IsChangedMethod(const MethodInfo* method);
	bool IsRemovedMethod(const MethodInfo* method);
	bool IsRemovedType(const Il2CppClass* klass);

    // Called by generated AOT entry guards to select changed methods without
    // replacing the AOT method pointer used by unchanged methods.
    bool ShouldDispatchToInterpreter(const MethodInfo* method);
    // A typed native entry must not interpret Current using an old value ABI.
    // Such calls must enter through a prepared Current frame instead.
    bool CanEnterWithBaseAbi(const MethodInfo* method);

    // Generated static calls may omit RuntimeMethod. Guards need only published
    // changed/tombstoned entries; they must never enumerate metadata during an
    // unchanged AOT call or before DHE loading (e.g. a SHA-256 bit operation).
    const MethodInfo* ResolveAotGuardMethodByToken(const char* assemblyName, uint32_t token);
    // Metadata preparation can still resolve an unpublished Base method.
    const MethodInfo* ResolveMethodByToken(const char* assemblyName, uint32_t token);
    const MethodInfo* ResolveMethodByNameAndToken(const char* assemblyName,
        const char* declaringType, const char* methodName, uint32_t parameterCount, uint32_t token);
    const MethodInfo* ResolveInterpreterMethod(const MethodInfo* baseMethod);
    // Interpreter call-site metadata needs the Current signature before stack
    // sizing. This only substitutes explicit physical Current bindings; it
    // does not raise tombstones while transforming an untaken call branch.
    const MethodInfo* ResolveCurrentExecutionMethod(const MethodInfo* method);
    // Direct bridge for supported generated native ABI shapes. It executes
    // current IL through Interpreter::Execute instead of calling
    // methodPointerCallByInterp, whose generated entry may be the AOT guard.
    int32_t ExecuteInterpreterI4I4(const MethodInfo* method, int32_t value);
    int32_t ExecuteInterpreterI4I4I4(const MethodInfo* method, int32_t left, int32_t right);
    int32_t ExecuteInterpreterPtrI4(const MethodInfo* method, void* pointer, int32_t value);
    int64_t ExecuteInterpreterI8I8(const MethodInfo* method, int64_t value);
    void ExecuteInterpreterVoidI4(const MethodInfo* method, int32_t value);
    void ExecuteInterpreterVoidNoArgs(const MethodInfo* method);
    void ExecuteInterpreterInstanceVoidNoArgs(const MethodInfo* method, void* thisPtr);
    void ExecuteInterpreterValueTypeInstanceVoidNoArgs(const MethodInfo* method, void* thisPtr);
    bool ExecuteInterpreterInstanceBool(const MethodInfo* method, void* thisPtr);
    void ExecuteInterpreterRefValueI4Ref(const MethodInfo* method, void* valuePtr, void* resultPtr);
    void ExecuteInterpreterValue(const MethodInfo* method, const void* value, uint32_t valueSize, void* result);
    // Generic generated entries expose their arguments through a mix of
    // value-storage addresses and raw invoke values. The kind array uses 0
    // for a generated value address and 1 for a raw reference/FGS value.
    void ExecuteInterpreterInvokeArgs(const MethodInfo* method, void* thisPtr,
        void** argumentValues, const uint8_t* argumentKinds, uint32_t argumentCount, void* result);
    int32_t ExecuteInterpreterInstanceI4I4(const MethodInfo* method, void* thisPtr, int32_t value);
    int64_t ExecuteInterpreterInstanceI8I8(const MethodInfo* method, void* thisPtr, int64_t value);
    void ExecuteInterpreterInstanceVoidI4(const MethodInfo* method, void* thisPtr, int32_t value);

    void RecordInterpreterEntry();
    void RecordAotBridgeCall();
    void RecordAotEntry();
    int32_t GetInterpreterEntryCount();
    int32_t GetAotBridgeCallCount();
    int32_t GetAotEntryCount();
    void ResetDispatchCounters();

    // Test-only reset. Production code never needs to remove a registered
    // DHE assembly because the corresponding homologous image is one-shot.
    void ResetForTests();
}
}
