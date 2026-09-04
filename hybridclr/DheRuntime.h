#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

struct Il2CppAssembly;
struct Il2CppClass;
struct MethodInfo;

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
        const std::vector<MetaVersionRegistration>& registrations);

    bool IsDheAssembly(const Il2CppAssembly* assembly);
    bool IsChangedMethod(const MethodInfo* method);
	bool IsRemovedMethod(const MethodInfo* method);
	bool IsRemovedType(const Il2CppClass* klass);

    // Called by generated AOT entry guards to select changed methods without
    // replacing the AOT method pointer used by unchanged methods.
    bool ShouldDispatchToInterpreter(const MethodInfo* method);

    // Unity's generated direct static calls may pass a null RuntimeMethod
    // context. Resolve the method from the loaded image so a token-only guard
    // can still select the interpreter implementation.
    const MethodInfo* ResolveMethodByToken(const char* assemblyName, uint32_t token);
    const MethodInfo* ResolveMethodByNameAndToken(const char* assemblyName,
        const char* declaringType, const char* methodName, uint32_t parameterCount, uint32_t token);
    const MethodInfo* ResolveInterpreterMethod(const MethodInfo* baseMethod);
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
