#include "DheRuntime.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "il2cpp-class-internals.h"
#include "vm/Assembly.h"
#include "vm/MetadataCache.h"
#include "vm/MetadataLock.h"
#include "vm/Class.h"
#include "vm/Image.h"
#include "interpreter/Interpreter.h"
#include "interpreter/InterpreterModule.h"
#include "interpreter/InterpreterDefs.h"
#include "metadata/GenericMetadata.h"
#include "metadata/AOTHomologousImage.h"
#include "vm/Exception.h"
#include "Il2CppCompatibleDef.h"

namespace hybridclr
{
namespace dhe
{
namespace
{
    constexpr char kMetaVersionMagic[] = "DHEMETA1";
    constexpr size_t kMetaVersionMagicSize = sizeof(kMetaVersionMagic) - 1;
    constexpr size_t kMetaVersionFixedHeaderSize = kMetaVersionMagicSize + 5 * sizeof(uint32_t) +
        kSha256DigestSize;
    constexpr size_t kMetaVersionTypeSize = 2 * kSha256DigestSize + 2 * sizeof(uint32_t);
    constexpr size_t kMetaVersionMethodSize = 3 * kSha256DigestSize + 2 * sizeof(uint32_t);

    struct DHEAssemblyState
    {
        CurrentImageSource source;
        std::unordered_set<uint32_t> changedMethodTokens;
		std::unordered_set<uint32_t> incompatibleBaseAbiTokens;
		std::unordered_set<uint32_t> removedTypeTokens;
        std::unordered_map<uint32_t, const MethodInfo*> resolvedMethods;
        std::unordered_map<uint32_t, const MethodInfo*> baseMethods;
        // Both Base and current-image MethodInfo pointers are normalized to
        // the immutable Base method token before the changed set is queried.
        std::unordered_map<const MethodInfo*, uint32_t> methodBaseTokens;
    };

    struct PublishedState
    {
        std::unordered_map<const Il2CppAssembly*, DHEAssemblyState> assemblyStates;
    };

    // State is copied and published only when an assembly is registered. The
    // old snapshots intentionally live for the process lifetime so an AOT
    // entry guard can read one without taking a lock or racing destruction.
    std::recursive_mutex s_registrationMutex;
    const PublishedState* s_emptyState = new PublishedState();
    std::atomic<const PublishedState*> s_publishedState{ s_emptyState };
    // SUPERSET creates the current MethodInfo objects before DHE receives the
    // MetaVersion pair. Keep these mappings until the registration snapshot
    // copies them into its lock-free dispatch state.
    std::unordered_map<const Il2CppAssembly*,
        std::unordered_map<const MethodInfo*, const MethodInfo*>> s_logicalMethodMappings;
    std::atomic<int32_t> s_interpreterEntryCount{ 0 };
    std::atomic<int32_t> s_aotBridgeCallCount{ 0 };
    std::atomic<int32_t> s_aotEntryCount{ 0 };

    constexpr uint32_t kSha256InitialState[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    constexpr uint32_t kSha256RoundConstants[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    inline uint32_t RotateRight(uint32_t value, uint32_t amount)
    {
        return (value >> amount) | (value << (32 - amount));
    }

    inline uint32_t ReadBigEndianU32(const uint8_t* data)
    {
        return (static_cast<uint32_t>(data[0]) << 24) |
            (static_cast<uint32_t>(data[1]) << 16) |
            (static_cast<uint32_t>(data[2]) << 8) |
            static_cast<uint32_t>(data[3]);
    }

    inline void WriteBigEndianU32(uint8_t* data, uint32_t value)
    {
        data[0] = static_cast<uint8_t>(value >> 24);
        data[1] = static_cast<uint8_t>(value >> 16);
        data[2] = static_cast<uint8_t>(value >> 8);
        data[3] = static_cast<uint8_t>(value);
    }

    void Sha256Transform(const uint8_t* block, uint32_t* state)
    {
        uint32_t words[64];
        for (uint32_t i = 0; i < 16; ++i)
        {
            words[i] = ReadBigEndianU32(block + i * 4);
        }
        for (uint32_t i = 16; i < 64; ++i)
        {
            uint32_t s0 = RotateRight(words[i - 15], 7) ^ RotateRight(words[i - 15], 18) ^ (words[i - 15] >> 3);
            uint32_t s1 = RotateRight(words[i - 2], 17) ^ RotateRight(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }

        uint32_t a = state[0];
        uint32_t b = state[1];
        uint32_t c = state[2];
        uint32_t d = state[3];
        uint32_t e = state[4];
        uint32_t f = state[5];
        uint32_t g = state[6];
        uint32_t h = state[7];
        for (uint32_t i = 0; i < 64; ++i)
        {
            uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            uint32_t ch = (e & f) ^ ((~e) & g);
            uint32_t temp1 = h + s1 + ch + kSha256RoundConstants[i] + words[i];
            uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    bool ReadU32(const uint8_t* data, size_t size, size_t& offset, uint32_t& value)
    {
        if (offset > size || size - offset < sizeof(uint32_t))
        {
            return false;
        }
        std::memcpy(&value, data + offset, sizeof(value));
        offset += sizeof(value);
        return true;
    }

    bool ReadDigest(const uint8_t* data, size_t size, size_t& offset, Sha256Digest& value)
    {
        if (offset > size || size - offset < value.size())
        {
            return false;
        }
        std::memcpy(value.data(), data + offset, value.size());
        offset += value.size();
        return true;
    }

    std::string DigestKey(const Sha256Digest& value)
    {
        return std::string(reinterpret_cast<const char*>(value.data()), value.size());
    }
}

bool ComputeSha256(const void* data, uint32_t size, Sha256Digest& result)
{
    if (!data && size != 0)
    {
        return false;
    }

    const size_t paddedSize = ((static_cast<size_t>(size) + 9u + 63u) / 64u) * 64u;
    std::vector<uint8_t> padded(paddedSize, 0);
    if (size != 0)
    {
        std::memcpy(padded.data(), data, size);
    }
    padded[size] = 0x80;
    const uint64_t bitLength = static_cast<uint64_t>(size) * 8u;
    for (uint32_t i = 0; i < 8; ++i)
    {
        padded[paddedSize - 1 - i] = static_cast<uint8_t>(bitLength >> (i * 8));
    }

    uint32_t state[8];
    std::memcpy(state, kSha256InitialState, sizeof(state));
    for (size_t offset = 0; offset < padded.size(); offset += 64)
    {
        Sha256Transform(padded.data() + offset, state);
    }
    for (uint32_t i = 0; i < 8; ++i)
    {
        WriteBigEndianU32(result.data() + i * 4, state[i]);
    }
    return true;
}

bool ParseMetaVersion(const void* data, uint32_t size, MetaVersionData& result)
{
    result = MetaVersionData{};
    if (!data || size < kMetaVersionFixedHeaderSize)
    {
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (std::memcmp(bytes, kMetaVersionMagic, kMetaVersionMagicSize) != 0)
    {
        return false;
    }

    size_t offset = kMetaVersionMagicSize;
    uint32_t schemaVersion = 0;
    uint32_t assemblyNameSize = 0;
    uint32_t typeCount = 0;
    uint32_t methodCount = 0;
    if (!ReadU32(bytes, size, offset, schemaVersion) ||
        !ReadU32(bytes, size, offset, result.flags) ||
        !ReadU32(bytes, size, offset, assemblyNameSize) ||
        !ReadU32(bytes, size, offset, typeCount) ||
        !ReadU32(bytes, size, offset, methodCount) ||
        !ReadDigest(bytes, size, offset, result.assemblyHash))
    {
        return false;
    }
    if (schemaVersion != kMetaVersionSchema || assemblyNameSize == 0 || assemblyNameSize > 1024 ||
        (result.flags & ~kMetaVersionKnownFlags) != 0 ||
        typeCount > (size - offset) / kMetaVersionTypeSize)
    {
        return false;
    }
    const size_t typeBytes = static_cast<size_t>(typeCount) * kMetaVersionTypeSize;
    if (size - offset < typeBytes || size - offset - typeBytes < assemblyNameSize ||
        methodCount > (size - offset - typeBytes - assemblyNameSize) / kMetaVersionMethodSize ||
        size - offset - typeBytes - assemblyNameSize !=
            static_cast<size_t>(methodCount) * kMetaVersionMethodSize)
    {
        return false;
    }

    result.assemblyName.assign(reinterpret_cast<const char*>(bytes + offset), assemblyNameSize);
    if (result.assemblyName.find('\0') != std::string::npos)
    {
        result = MetaVersionData{};
        return false;
    }
    offset += assemblyNameSize;

    std::unordered_set<std::string> typeIds;
    std::unordered_set<uint32_t> typeTokens;
    result.types.reserve(typeCount);
    for (uint32_t index = 0; index < typeCount; ++index)
    {
        MetaVersionType type;
        if (!ReadDigest(bytes, size, offset, type.stableId) ||
            !ReadDigest(bytes, size, offset, type.version) ||
            !ReadU32(bytes, size, offset, type.token) ||
            !ReadU32(bytes, size, offset, type.flags) ||
            (type.token & 0xff000000u) != 0x02000000u ||
            (type.flags & ~kMetaVersionKnownTypeFlags) != 0 ||
            !typeIds.insert(DigestKey(type.stableId)).second ||
            !typeTokens.insert(type.token).second)
        {
            result = MetaVersionData{};
            return false;
        }
        result.types.push_back(type);
    }

    std::unordered_set<std::string> methodIds;
    std::unordered_set<uint32_t> methodTokens;
    result.methods.reserve(methodCount);
    for (uint32_t index = 0; index < methodCount; ++index)
    {
        MetaVersionMethod method;
        if (!ReadDigest(bytes, size, offset, method.stableId) ||
            !ReadDigest(bytes, size, offset, method.version) ||
            !ReadDigest(bytes, size, offset, method.declaringTypeStableId) ||
            !ReadU32(bytes, size, offset, method.token) ||
            !ReadU32(bytes, size, offset, method.flags) ||
            (method.token & 0xff000000u) != 0x06000000u ||
            (method.flags & ~kMetaVersionKnownMethodFlags) != 0 ||
            typeIds.find(DigestKey(method.declaringTypeStableId)) == typeIds.end() ||
            !methodIds.insert(DigestKey(method.stableId)).second ||
            !methodTokens.insert(method.token).second)
        {
            result = MetaVersionData{};
            return false;
        }
        result.methods.push_back(method);
    }
    return offset == size;
}

    bool PrepareDheInterpreterMethod(const MethodInfo* method)
    {
        if (!method)
        {
            return false;
        }
        MethodInfo* mutableMethod = const_cast<MethodInfo*>(method);
        const bool wasInterpreterMethod = mutableMethod->isInterpterImpl;
        // GetInterpMethodInfo requires the interpreter bit while it builds the
        // transformed body. If transformation fails, restore that bit before
        // returning so a failed guard cannot masquerade as a prepared method.
        if (!mutableMethod->interpData)
        {
            mutableMethod->isInterpterImpl = true;
            if (!hybridclr::interpreter::InterpreterModule::GetInterpMethodInfo(mutableMethod))
            {
                mutableMethod->isInterpterImpl = wasInterpreterMethod;
                return false;
            }
        }
        mutableMethod->isInterpterImpl = true;
        RecordInterpreterEntry();
        return mutableMethod->interpData != nullptr;
    }

    void RequireDheInterpreterMethod(const MethodInfo* method)
    {
        if (!PrepareDheInterpreterMethod(method))
        {
            il2cpp::vm::Exception::Raise(
                il2cpp::vm::Exception::GetExecutionEngineException(
                    "DHE interpreter method preparation failed"));
        }
    }

    struct MethodPreparationSnapshot
    {
        MethodInfo* method = nullptr;
        Il2CppMethodPointer methodPointer = nullptr;
        Il2CppMethodPointer virtualMethodPointer = nullptr;
        InvokerMethod invokerMethod = nullptr;
        bool initInterpCallMethodPointer = false;
        bool isInterpterImpl = false;
        bool hasFullGenericSharingAotInvoker = false;
        void* interpData = nullptr;
        Il2CppMethodPointer methodPointerCallByInterp = nullptr;
        Il2CppMethodPointer virtualMethodPointerCallByInterp = nullptr;
#if HYBRIDCLR_UNITY_2021_OR_NEW
        uint32_t fullGenericSharingPreparationState = 0;
#endif
        VirtualInvokeData* vtableEntry = nullptr;
        const MethodInfo* vtableMethod = nullptr;
        Il2CppMethodPointer vtableMethodPointer = nullptr;
    };

    MethodPreparationSnapshot CaptureMethodPreparationSnapshot(const MethodInfo* method)
    {
        MethodPreparationSnapshot snapshot;
        snapshot.method = const_cast<MethodInfo*>(method);
        snapshot.methodPointer = method->methodPointer;
        snapshot.virtualMethodPointer = method->virtualMethodPointer;
        snapshot.invokerMethod = method->invoker_method;
        snapshot.initInterpCallMethodPointer = method->initInterpCallMethodPointer;
        snapshot.isInterpterImpl = method->isInterpterImpl;
        snapshot.hasFullGenericSharingAotInvoker = method->hasFullGenericSharingAotInvoker;
        snapshot.interpData = method->interpData;
        snapshot.methodPointerCallByInterp = method->methodPointerCallByInterp;
        snapshot.virtualMethodPointerCallByInterp = method->virtualMethodPointerCallByInterp;
#if HYBRIDCLR_UNITY_2021_OR_NEW
        snapshot.fullGenericSharingPreparationState = method->fullGenericSharingPreparationState;
#endif
        if (method->klass && method->slot < method->klass->vtable_count && method->klass->vtable)
        {
            snapshot.vtableEntry = &method->klass->vtable[method->slot];
            snapshot.vtableMethod = snapshot.vtableEntry->method;
            snapshot.vtableMethodPointer = snapshot.vtableEntry->methodPtr;
        }
        return snapshot;
    }

    void RestoreMethodPreparationSnapshot(const MethodPreparationSnapshot& snapshot)
    {
        if (!snapshot.method)
        {
            return;
        }
        snapshot.method->methodPointer = snapshot.methodPointer;
        snapshot.method->virtualMethodPointer = snapshot.virtualMethodPointer;
        snapshot.method->invoker_method = snapshot.invokerMethod;
        snapshot.method->initInterpCallMethodPointer = snapshot.initInterpCallMethodPointer;
        snapshot.method->isInterpterImpl = snapshot.isInterpterImpl;
        snapshot.method->hasFullGenericSharingAotInvoker = snapshot.hasFullGenericSharingAotInvoker;
        snapshot.method->interpData = snapshot.interpData;
        snapshot.method->methodPointerCallByInterp = snapshot.methodPointerCallByInterp;
        snapshot.method->virtualMethodPointerCallByInterp = snapshot.virtualMethodPointerCallByInterp;
#if HYBRIDCLR_UNITY_2021_OR_NEW
        snapshot.method->fullGenericSharingPreparationState = snapshot.fullGenericSharingPreparationState;
#endif
        if (snapshot.vtableEntry)
        {
            snapshot.vtableEntry->method = snapshot.vtableMethod;
            snapshot.vtableEntry->methodPtr = snapshot.vtableMethodPointer;
        }
    }

bool IsDheAssembly(const Il2CppAssembly* assembly)
{
    if (!assembly)
    {
        return false;
    }
    const PublishedState* state = s_publishedState.load(std::memory_order_acquire);
    return state->assemblyStates.find(assembly) != state->assemblyStates.end();
}

bool IsMutableDheAssembly(const Il2CppAssembly* assembly)
{
    const PublishedState* state = s_publishedState.load(std::memory_order_acquire);
    auto entry = state->assemblyStates.find(assembly);
    return entry != state->assemblyStates.end() &&
        entry->second.source.kind == CurrentImageSourceKind::MutableHotfix;
}

bool IsDheModuleInitializationReady(const char* assemblyName)
{
    return assemblyName && assemblyName[0] && IsMutableDheAssembly(
        il2cpp::vm::MetadataCache::GetAssemblyByName(assemblyName));
}

bool IsFrozenAotExecutionSource(const Il2CppAssembly* assembly)
{
    const PublishedState* state = s_publishedState.load(std::memory_order_acquire);
    auto entry = state->assemblyStates.find(assembly);
    return entry != state->assemblyStates.end() &&
        entry->second.source.kind == CurrentImageSourceKind::FrozenBaseAot;
}

bool RegisterLogicalMethodMapping(const Il2CppAssembly* assembly,
    const MethodInfo* currentMethod, const MethodInfo* baseMethod)
{
    if (!assembly || !currentMethod || !baseMethod || currentMethod == baseMethod)
    {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(s_registrationMutex);
    auto& mappings = s_logicalMethodMappings[assembly];
    auto existing = mappings.find(currentMethod);
    if (existing != mappings.end() && existing->second != baseMethod)
    {
        return false;
    }
    mappings[currentMethod] = baseMethod;
    return true;
}

static bool IsUnaffectedFrozenGenericInstance(const MethodInfo* method,
    const DHEAssemblyState& state, uint32_t token);

bool IsChangedMethod(const MethodInfo* method)
{
    if (!method || !method->klass || !method->klass->image || !method->klass->image->assembly)
    {
        return false;
    }
    const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
    auto state = published->assemblyStates.find(method->klass->image->assembly);
    if (state == published->assemblyStates.end())
    {
        return false;
    }
    const MethodInfo* definition = method->is_inflated && method->genericMethod &&
        method->genericMethod->methodDefinition
        ? method->genericMethod->methodDefinition
        : method;
    auto identity = state->second.methodBaseTokens.find(definition);
    if (identity == state->second.methodBaseTokens.end())
    {
        return false;
    }
    const uint32_t token = identity->second;
    if (IsUnaffectedFrozenGenericInstance(method, state->second, token)) return false;
    return state->second.baseMethods.find(token) != state->second.baseMethods.end() &&
        state->second.changedMethodTokens.find(token) !=
            state->second.changedMethodTokens.end();
}

bool IsRemovedMethod(const MethodInfo* method)
{
	if (!method || !method->klass || !method->klass->image ||
		!method->klass->image->assembly)
	{
		return false;
	}
	const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
	auto state = published->assemblyStates.find(method->klass->image->assembly);
	if (state == published->assemblyStates.end())
	{
		return false;
	}
	const MethodInfo* definition = method->is_inflated && method->genericMethod &&
		method->genericMethod->methodDefinition
		? method->genericMethod->methodDefinition
		: method;
    auto identity = state->second.methodBaseTokens.find(definition);
    if (identity == state->second.methodBaseTokens.end())
    {
        return false;
    }
    const uint32_t token = identity->second;
    auto baseMethod = state->second.baseMethods.find(token);
	if (baseMethod == state->second.baseMethods.end() || baseMethod->second != definition)
	{
		return false;
	}
	auto resolved = state->second.resolvedMethods.find(token);
	return resolved != state->second.resolvedMethods.end() && resolved->second == nullptr;
}

bool IsRemovedType(const Il2CppClass* klass)
{
	if (!klass || !klass->image || !klass->image->assembly)
	{
		return false;
	}
	const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
	auto state = published->assemblyStates.find(klass->image->assembly);
	return state != published->assemblyStates.end() &&
		state->second.removedTypeTokens.find(klass->token) !=
		state->second.removedTypeTokens.end();
}

bool CanEnterWithBaseAbi(const MethodInfo* method)
{
    if (!method || !method->klass || !method->klass->image)
        return true;
    const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
    auto state = published->assemblyStates.find(method->klass->image->assembly);
    if (state == published->assemblyStates.end())
        return true;
    const MethodInfo* definition = method->is_inflated && method->genericMethod
        ? method->genericMethod->methodDefinition : method;
    auto identity = state->second.methodBaseTokens.find(definition);
    if (identity == state->second.methodBaseTokens.end())
        return true;
    if (IsUnaffectedFrozenGenericInstance(method, state->second, identity->second)) return true;
    auto native = state->second.baseMethods.find(identity->second);
    // Current execution metadata already describes a Current frame.
    return native == state->second.baseMethods.end() || native->second != definition ||
        state->second.incompatibleBaseAbiTokens.find(identity->second) ==
            state->second.incompatibleBaseAbiTokens.end();
}

bool ShouldDispatchToInterpreter(const MethodInfo* method)
{
    if (!IsChangedMethod(method))
        return false;
    if (!CanEnterWithBaseAbi(method))
        il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetExecutionEngineException(
            "DHE Current value layout requires a Current call frame; the old AOT ABI cannot be used."));
    return true;
}

static const MethodInfo* ResolveMethodInAssembly(const Il2CppAssembly* assembly, uint32_t token)
{
    if (!assembly || token == 0)
    {
        return nullptr;
    }
    const Il2CppImage* image = il2cpp::vm::Assembly::GetImage(assembly);
    if (!image)
    {
        return nullptr;
    }

    // Resolve against the physical Base definition table. The reflection
    // enumeration hides <Module> and can include Current supplemental types;
    // neither behavior is valid for an assembly-local Base MethodDef token.
    const uint32_t typeCount = il2cpp::vm::Image::GetNumTypes(image);
    for (uint32_t index = 0; index < typeCount; ++index)
    {
        const Il2CppClass* type = il2cpp::vm::Image::GetType(image, index);
        if (!type)
        {
            continue;
        }
        Il2CppClass* mutableType = const_cast<Il2CppClass*>(type);
        il2cpp::vm::Class::SetupMethods(mutableType);
        for (uint16_t i = 0; i < mutableType->method_count; ++i)
        {
            const MethodInfo* method = mutableType->methods[i];
            if (method && method->token == token)
            {
                return method;
            }
        }
    }
    return nullptr;
}

const MethodInfo* ResolveAotGuardMethodByToken(const char* assemblyName, uint32_t token)
{
    if (!assemblyName || !assemblyName[0] || (token >> 24) != 6 || (token & 0xffffffu) == 0)
        return nullptr;
    const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
    for (const auto& entry : published->assemblyStates)
    {
        const Il2CppAssembly* assembly = entry.first;
        if (!assembly || !assembly->aname.name || std::strcmp(assembly->aname.name, assemblyName) != 0)
            continue;
        if (entry.second.changedMethodTokens.find(token) == entry.second.changedMethodTokens.end())
            return nullptr;
        auto method = entry.second.baseMethods.find(token);
        return method == entry.second.baseMethods.end() ? nullptr : method->second;
    }
    return nullptr;
}

const MethodInfo* ResolveMethodByToken(const char* assemblyName, uint32_t token)
{
    if (!assemblyName || assemblyName[0] == '\0' || token == 0)
    {
        return nullptr;
    }
    const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
    for (const auto& entry : published->assemblyStates)
    {
        const Il2CppAssembly* assembly = entry.first;
        if (!assembly || !assembly->aname.name || std::strcmp(assembly->aname.name, assemblyName) != 0)
        {
            continue;
        }
        auto baseMethod = entry.second.baseMethods.find(token);
        if (baseMethod != entry.second.baseMethods.end())
        {
            return baseMethod->second;
        }
    }
    return ResolveMethodInAssembly(il2cpp::vm::MetadataCache::GetAssemblyByName(assemblyName), token);
}

// Generic MethodInfo instances are keyed by their declaring definition plus
// the concrete class/method instantiations.  A Base Player instantiation can
// contain value types whose physical representation changed in the Current
// image; inflating the Current definition with the original context then
// silently reintroduces the Base layout.  Remap each concrete argument through
// its owning DHE image before inflating the Current method.
static const Il2CppType* RemapDheGenericArgument(const Il2CppType* type,
    const Il2CppAssembly* preferredAssembly)
{
    if (!type)
    {
        return nullptr;
    }

    if (preferredAssembly && IsDheAssembly(preferredAssembly))
    {
        metadata::AOTHomologousImage* preferred =
            metadata::AOTHomologousImage::FindImageByAssembly(preferredAssembly);
        if (preferred)
        {
            if (const Il2CppType* mapped = preferred->GetDheExecutionType(type))
                return mapped;
        }
    }

    Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
    if (!klass || !klass->image || !klass->image->assembly)
    {
        return nullptr;
    }

    if (!IsDheAssembly(klass->image->assembly)) return nullptr;
    metadata::AOTHomologousImage* image =
        metadata::AOTHomologousImage::FindImageByAssembly(klass->image->assembly);
    return image ? image->GetDheExecutionType(type) : nullptr;
}

static Il2CppGenericContext RemapDheGenericContext(const Il2CppGenericContext& baseContext,
    const Il2CppAssembly* preferredAssembly, bool& changed)
{
    Il2CppGenericContext currentContext = baseContext;
    auto remapInst = [&changed, preferredAssembly](const Il2CppGenericInst* baseInst) -> const Il2CppGenericInst*
    {
        if (!baseInst || baseInst->type_argc == 0)
        {
            return baseInst;
        }

        std::vector<const Il2CppType*> mapped(baseInst->type_argc);
        bool instChanged = false;
        for (uint32_t i = 0; i < baseInst->type_argc; ++i)
        {
            const Il2CppType* argument = baseInst->type_argv[i];
            const Il2CppType* mappedArgument = RemapDheGenericArgument(argument, preferredAssembly);
            mapped[i] = mappedArgument ? mappedArgument : argument;
            instChanged |= mapped[i] != argument;
        }

        if (!instChanged)
        {
            return baseInst;
        }
        changed = true;
        return il2cpp::vm::MetadataCache::GetGenericInst(mapped.data(), baseInst->type_argc);
    };

    currentContext.class_inst = remapInst(baseContext.class_inst);
    currentContext.method_inst = remapInst(baseContext.method_inst);
    return currentContext;
}

static bool GenericArgumentNeedsCurrent(const Il2CppType* type, const Il2CppAssembly* assembly)
{
    if (!type) return true; // Unknown contexts cannot opt into the native path.
    switch (type->type)
    {
    case IL2CPP_TYPE_BOOLEAN: case IL2CPP_TYPE_CHAR:
    case IL2CPP_TYPE_I1: case IL2CPP_TYPE_U1: case IL2CPP_TYPE_I2: case IL2CPP_TYPE_U2:
    case IL2CPP_TYPE_I4: case IL2CPP_TYPE_U4: case IL2CPP_TYPE_I8: case IL2CPP_TYPE_U8:
    case IL2CPP_TYPE_R4: case IL2CPP_TYPE_R8: case IL2CPP_TYPE_I: case IL2CPP_TYPE_U:
    case IL2CPP_TYPE_STRING: case IL2CPP_TYPE_OBJECT:
        return false;
    case IL2CPP_TYPE_SZARRAY: case IL2CPP_TYPE_PTR:
        return GenericArgumentNeedsCurrent(type->data.type, assembly);
    case IL2CPP_TYPE_ARRAY:
        return GenericArgumentNeedsCurrent(type->data.array->etype, assembly);
    case IL2CPP_TYPE_GENERICINST:
    {
        const Il2CppGenericClass* generic = type->data.generic_class;
        if (GenericArgumentNeedsCurrent(generic->type, assembly)) return true;
        const Il2CppGenericInst* inst = generic->context.class_inst;
        if (!inst) return true;
        for (uint32_t index = 0; index < inst->type_argc; ++index)
            if (GenericArgumentNeedsCurrent(inst->type_argv[index], assembly)) return true;
        return false;
    }
    case IL2CPP_TYPE_CLASS: case IL2CPP_TYPE_VALUETYPE:
    {
        Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
        // An interpreter argument already has its Current identity; remapping
        // alone would return the same pointer and miss this dependency.
        if (!klass || metadata::IsInterpreterImage(klass->image)) return true;
        const Il2CppType* current = RemapDheGenericArgument(type, assembly);
        return current && current != type;
    }
    default:
        return true;
    }
}

static bool IsUnaffectedFrozenGenericInstance(const MethodInfo* method,
    const DHEAssemblyState& state, uint32_t token)
{
    if (state.source.kind != CurrentImageSourceKind::FrozenBaseAot ||
        !std::binary_search(state.source.genericContextMethodTokens.begin(),
            state.source.genericContextMethodTokens.end(), token) ||
        !method->is_inflated || !method->genericMethod)
        return false;
    auto base = state.baseMethods.find(token);
    if (base == state.baseMethods.end() || base->second != method->genericMethod->methodDefinition)
        return false; // Physical Current methods remain Current.
    const Il2CppGenericContext& context = method->genericMethod->context;
    if (!context.class_inst && !context.method_inst) return false;
    for (const Il2CppGenericInst* inst : { context.class_inst, context.method_inst })
        if (inst)
            for (uint32_t index = 0; index < inst->type_argc; ++index)
                if (GenericArgumentNeedsCurrent(inst->type_argv[index], method->klass->image->assembly))
                    return false;
    return true;
}

const MethodInfo* ResolveInterpreterMethod(const MethodInfo* baseMethod)
{
    if (!baseMethod || !baseMethod->klass || !baseMethod->klass->image ||
        !baseMethod->klass->image->assembly)
    {
        return nullptr;
    }
    const MethodInfo* baseDefinition = baseMethod->is_inflated && baseMethod->genericMethod &&
        baseMethod->genericMethod->methodDefinition
        ? baseMethod->genericMethod->methodDefinition
        : baseMethod;
    const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
    auto state = published->assemblyStates.find(baseMethod->klass->image->assembly);
    if (state == published->assemblyStates.end())
    {
        return baseMethod;
    }
	auto identity = state->second.methodBaseTokens.find(baseDefinition);
	if (identity == state->second.methodBaseTokens.end())
	{
		return baseMethod;
	}
	const uint32_t token = identity->second;
	if (IsUnaffectedFrozenGenericInstance(baseMethod, state->second, token)) return baseMethod;
	if (state->second.changedMethodTokens.find(token) == state->second.changedMethodTokens.end())
	{
		return baseMethod;
	}
	auto registeredBaseMethod = state->second.baseMethods.find(token);
	if (registeredBaseMethod == state->second.baseMethods.end() ||
		registeredBaseMethod->second != baseDefinition)
	{
		return baseMethod;
	}
    auto current = state->second.resolvedMethods.find(token);
    if (current == state->second.resolvedMethods.end() || !current->second)
    {
        il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetMissingMethodException(
            "A DHE method present in the Base Player was removed by the current update."));
        return nullptr;
    }
    const MethodInfo* currentMethod = current->second;
    if (baseMethod->is_inflated && baseMethod->genericMethod)
    {
        bool contextChanged = false;
        Il2CppGenericContext currentContext = RemapDheGenericContext(
            baseMethod->genericMethod->context, baseMethod->klass->image->assembly, contextChanged);
        currentMethod = il2cpp::metadata::GenericMetadata::Inflate(currentMethod,
            contextChanged ? &currentContext : &baseMethod->genericMethod->context);
        if (currentMethod)
        {
            // Full generic sharing normally keeps the generated AOT entry even
            // when the definition is selected. A value-type layout change
            // makes that entry unsafe for this concrete instantiation, so
            // materialize the interpreter bridge on the inflated MethodInfo.
            hybridclr::InitAndGetInterpreterDirectlyCallMethodPointer(currentMethod);
            const_cast<MethodInfo*>(currentMethod)->isInterpterImpl = true;
        }
    }
    return currentMethod;
}

const MethodInfo* ResolveMethodByNameAndToken(const char* assemblyName,
    const char* declaringType, const char* methodName, uint32_t parameterCount, uint32_t token)
{
    if (!assemblyName || assemblyName[0] == '\0' || !declaringType || declaringType[0] == '\0' ||
        !methodName || methodName[0] == '\0' || token == 0)
    {
        return nullptr;
    }

    const Il2CppAssembly* assembly = il2cpp::vm::MetadataCache::GetAssemblyByName(assemblyName);
    const Il2CppImage* image = assembly ? il2cpp::vm::Assembly::GetImage(assembly) : nullptr;
    if (!image)
    {
        return nullptr;
    }

    std::string typePath(declaringType);
    const size_t separator = typePath.find_last_of('.');
    const std::string nameSpace = separator == std::string::npos ? std::string() : typePath.substr(0, separator);
    const std::string typeName = separator == std::string::npos ? typePath : typePath.substr(separator + 1);
    Il2CppClass* klass = il2cpp::vm::Image::ClassFromName(image, nameSpace.c_str(), typeName.c_str());
    if (!klass)
    {
        return nullptr;
    }

    const MethodInfo* method = il2cpp::vm::Class::GetMethodFromName(klass, methodName, static_cast<int>(parameterCount));
    return method && method->token == token ? method : nullptr;
}

const MethodInfo* ResolveCurrentExecutionMethod(const MethodInfo* method)
{
    if (!method || !method->klass || !method->klass->image) return method;
    const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
    auto state = published->assemblyStates.find(method->klass->image->assembly);
    if (state == published->assemblyStates.end()) return method;
    const MethodInfo* definition = method->is_inflated && method->genericMethod
        ? method->genericMethod->methodDefinition : method;
    auto identity = state->second.methodBaseTokens.find(definition);
    if (identity == state->second.methodBaseTokens.end()) return method;
    if (IsUnaffectedFrozenGenericInstance(method, state->second, identity->second)) return method;
    auto base = state->second.baseMethods.find(identity->second);
    auto current = state->second.resolvedMethods.find(identity->second);
    if (base == state->second.baseMethods.end() || base->second != definition ||
        current == state->second.resolvedMethods.end() || !current->second || current->second == definition)
        return method;
    if (method->is_inflated && method->genericMethod)
    {
        bool contextChanged = false;
        Il2CppGenericContext currentContext = RemapDheGenericContext(
            method->genericMethod->context, method->klass->image->assembly, contextChanged);
        const MethodInfo* execution = il2cpp::metadata::GenericMetadata::Inflate(current->second,
            contextChanged ? &currentContext : &method->genericMethod->context);
        if (execution)
        {
            hybridclr::InitAndGetInterpreterDirectlyCallMethodPointer(execution);
            const_cast<MethodInfo*>(execution)->isInterpterImpl = true;
        }
        return execution;
    }
    return current->second;
}

static void RollbackMethodPreparations(
    const std::vector<MethodPreparationSnapshot>& snapshots)
{
    for (auto it = snapshots.rbegin(); it != snapshots.rend(); ++it)
    {
        RestoreMethodPreparationSnapshot(*it);
    }
}

static void CommitMethodPreparations(
    const std::vector<MethodPreparationSnapshot>& snapshots)
{
    for (const MethodPreparationSnapshot& snapshot : snapshots)
    {
        if (snapshot.vtableEntry)
        {
            snapshot.vtableEntry->method = snapshot.method;
            snapshot.vtableEntry->methodPtr =
                snapshot.method->virtualMethodPointerCallByInterp;
        }
    }
}

static bool PrepareResolvedMethods(const std::vector<const MethodInfo*>& methods,
    std::vector<MethodPreparationSnapshot>& snapshots)
{
    for (const MethodInfo* method : methods)
    {
        if (!method)
        {
            return false;
        }

        // Methods declared on generic classes do not have a concrete
        // direct-call ABI until an inflated instantiation is used; leave
        // those shapes to the normal interpreter metadata path. Generic
        // method definitions themselves still need an interpreter bridge so
        // a changed value-type instantiation cannot fall back to the Base ABI.
        if (method->klass && (method->klass->generic_class || method->klass->genericContainerHandle))
        {
            continue;
        }

        snapshots.push_back(CaptureMethodPreparationSnapshot(method));
        Il2CppMethodPointer interpreterPointer =
            hybridclr::InitAndGetInterpreterDirectlyCallMethodPointer(method);
        if (!interpreterPointer)
        {
            return false;
        }
        // The direct-call pointer can have been initialized by an earlier
        // interpreter bridge. DHE still owns the dispatch decision for this
        // token, so make the interpreter implementation bit explicit.
        const_cast<MethodInfo*>(method)->isInterpterImpl = true;
    }
    return true;
}

bool PrepareChangedMethods(const Il2CppAssembly* assembly,
    const std::vector<uint32_t>& changedMethodTokens,
    std::vector<const MethodInfo*>& resolvedMethods)
{
    // Preparation mutates MethodInfo and vtable state. Serialize it with the
    // registration commit so a concurrent failed load cannot restore a
    // snapshot over a successful load's published interpreter pointers.
    std::lock_guard<std::recursive_mutex> lock(s_registrationMutex);
    if (!assembly || !assembly->aname.name || IsDheAssembly(assembly))
    {
        return false;
    }

    resolvedMethods.clear();
    resolvedMethods.reserve(changedMethodTokens.size());
    for (uint32_t token : changedMethodTokens)
    {
        const MethodInfo* method = ResolveMethodByToken(assembly->aname.name, token);
        if (!method)
        {
            resolvedMethods.clear();
            return false;
        }
        resolvedMethods.push_back(method);
    }

    std::vector<MethodPreparationSnapshot> snapshots;
    snapshots.reserve(resolvedMethods.size());
    try
    {
        if (!PrepareResolvedMethods(resolvedMethods, snapshots))
        {
            RollbackMethodPreparations(snapshots);
            resolvedMethods.clear();
            return false;
        }
    }
    catch (...)
    {
        RollbackMethodPreparations(snapshots);
        resolvedMethods.clear();
        throw;
    }
    CommitMethodPreparations(snapshots);
    return true;
}

static bool MethodCanHaveAotEntry(const MetaVersionMethod& method)
{
    constexpr uint32_t kAbstract = 2u;
    constexpr uint32_t kPInvoke = 4u;
    constexpr uint32_t kHasBody = 8u;
    return (method.flags & kHasBody) != 0 && (method.flags & (kAbstract | kPInvoke)) == 0;
}

bool ValidateCurrentImageSource(const CurrentImageSource& source,
    const Sha256Digest& baseHash, const Sha256Digest& currentHash)
{
    if (source.kind == CurrentImageSourceKind::MutableHotfix)
        return source.baseSourceHash == Sha256Digest{} && source.excludedBaseTypeTokens.empty() &&
            source.genericContextMethodTokens.empty();
    if (source.kind != CurrentImageSourceKind::FrozenBaseAot ||
        source.baseSourceHash == Sha256Digest{} || source.baseSourceHash != baseHash || baseHash != currentHash)
        return false;
    uint32_t previous = 0;
    for (uint32_t token : source.excludedBaseTypeTokens)
    {
        if ((token >> 24) != 2 || (token & 0xffffffu) <= 1 || token <= previous) return false;
        previous = token;
    }
    previous = 0;
    for (uint32_t token : source.genericContextMethodTokens)
    {
        if ((token >> 24) != 6 || (token & 0xffffffu) == 0 || token <= previous) return false;
        previous = token;
    }
    return true;
}

static bool SameFrozenMetaVersion(const MetaVersionData& base, const MetaVersionData& current)
{
    // MV bytes are canonical. Require the same ordered records as well as the
    // same DLL digest; a payload cannot disguise changed code with an old hash.
    if (base.assemblyName != current.assemblyName || base.flags != current.flags ||
        base.assemblyHash != current.assemblyHash || base.types.size() != current.types.size() ||
        base.methods.size() != current.methods.size()) return false;
    for (size_t index = 0; index < base.types.size(); ++index)
    {
        const MetaVersionType& a = base.types[index];
        const MetaVersionType& b = current.types[index];
        if (a.stableId != b.stableId || a.version != b.version || a.token != b.token || a.flags != b.flags)
            return false;
    }
    for (size_t index = 0; index < base.methods.size(); ++index)
    {
        const MetaVersionMethod& a = base.methods[index];
        const MetaVersionMethod& b = current.methods[index];
        if (a.stableId != b.stableId || a.version != b.version || a.token != b.token || a.flags != b.flags ||
            a.declaringTypeStableId != b.declaringTypeStableId) return false;
    }
    return true;
}

bool BuildCurrentImagePlan(const MetaVersionData& baseMetaVersion,
    const MetaVersionData& currentMetaVersion,
    const std::vector<uint32_t>& currentTypeTokens,
    const std::vector<uint32_t>& currentMethodTokens, CurrentImagePlan& result,
    const CurrentImageSource& source)
{
    if (baseMetaVersion.assemblyName.empty() || baseMetaVersion.assemblyName != currentMetaVersion.assemblyName ||
        !ValidateCurrentImageSource(source, baseMetaVersion.assemblyHash, currentMetaVersion.assemblyHash) ||
        (source.kind == CurrentImageSourceKind::FrozenBaseAot && !SameFrozenMetaVersion(baseMetaVersion, currentMetaVersion)))
        return false;
    std::unordered_map<std::string, const MetaVersionType*> baseTypes;
    std::unordered_map<std::string, const MetaVersionMethod*> baseMethods;
    std::unordered_map<uint32_t, const MetaVersionType*> currentTypes;
    std::unordered_map<uint32_t, const MetaVersionMethod*> currentMethods;
    static const Sha256Digest moduleIdentity = []() {
        Sha256Digest value{};
        const char name[] = "dhe-type-id\n<Module>";
        ComputeSha256(name, sizeof(name) - 1, value);
        return value;
    }();
    for (uint32_t side = 0; side < 2; ++side)
    {
        const MetaVersionData* mv = side == 0 ? &baseMetaVersion : &currentMetaVersion;
        std::unordered_set<std::string> typeIds, methodIds;
        std::unordered_set<uint32_t> typeTokens, methodTokens;
        for (const MetaVersionType& type : mv->types)
        {
            const std::string id = DigestKey(type.stableId);
            if ((type.token >> 24) != 2 || (type.token & 0xffffffu) == 0 ||
                (type.token == 0x02000001 && (type.flags != 0 || type.stableId != moduleIdentity)) ||
                (type.flags & ~kMetaVersionKnownTypeFlags) ||
                !typeIds.insert(id).second || !typeTokens.insert(type.token).second)
                return false;
            if (side == 0) baseTypes.emplace(id, &type);
            else currentTypes.emplace(type.token, &type);
        }
        for (const MetaVersionMethod& method : mv->methods)
        {
            const std::string id = DigestKey(method.stableId);
            if ((method.token >> 24) != 6 || (method.token & 0xffffffu) == 0 ||
                (method.flags & ~kMetaVersionKnownMethodFlags) ||
                !methodIds.insert(id).second || !methodTokens.insert(method.token).second ||
                typeIds.find(DigestKey(method.declaringTypeStableId)) == typeIds.end())
                return false;
            if (side == 0) baseMethods.emplace(id, &method);
            else currentMethods.emplace(method.token, &method);
        }
    }
    CurrentImagePlan plan;
    plan.source = source;
    plan.assemblyName = baseMetaVersion.assemblyName;
    plan.baseAssemblyHash = baseMetaVersion.assemblyHash;
    plan.currentAssemblyHash = currentMetaVersion.assemblyHash;
    std::unordered_set<std::string> selectedTypes;
    std::unordered_set<std::string> excludedTypes;
    for (uint32_t token : source.excludedBaseTypeTokens)
    {
        auto entry = currentTypes.find(token); // Frozen MV has identical Base tokens.
        if (entry == currentTypes.end()) return false;
        excludedTypes.insert(DigestKey(entry->second->stableId));
    }
    std::unordered_set<uint32_t> selectedMethods;
    for (uint32_t token : currentTypeTokens)
    {
        if (token == 0x02000001) return false; // Global methods have no instance storage.
        auto entry = currentTypes.find(token);
        if (entry == currentTypes.end()) return false;
        const MetaVersionType& type = *entry->second;
        const std::string id = DigestKey(type.stableId);
        auto old = baseTypes.find(id);
        if (old == baseTypes.end() || old->second->flags != type.flags ||
            excludedTypes.count(id) || !selectedTypes.insert(id).second)
            return false;
        plan.types.emplace_back(old->second->token, token);
    }
    for (uint32_t token : currentMethodTokens)
    {
        auto entry = currentMethods.find(token);
        if (entry == currentMethods.end() || !selectedMethods.insert(token).second)
            return false;
    }
    for (uint32_t token : source.genericContextMethodTokens)
    {
        auto method = currentMethods.find(token);
        if (!selectedMethods.count(token) || method == currentMethods.end() ||
            selectedTypes.count(DigestKey(method->second->declaringTypeStableId))) return false;
    }
    for (const MetaVersionMethod& method : currentMetaVersion.methods)
    {
        auto old = baseMethods.find(DigestKey(method.stableId));
        const bool explicitSelection = selectedMethods.find(method.token) != selectedMethods.end();
        const bool storageMember = selectedTypes.find(DigestKey(method.declaringTypeStableId)) != selectedTypes.end();
        if (!explicitSelection && (!storageMember || old == baseMethods.end())) continue;
        if (excludedTypes.count(DigestKey(method.declaringTypeStableId))) return false;
        if (old == baseMethods.end() || old->second->declaringTypeStableId != method.declaringTypeStableId)
            return false;
        // Abstract members do not execute or store a receiver. Native-only
        // members need a separately verified ABI bridge, even on a selected type.
        if (!explicitSelection && (method.flags & 2u) && (old->second->flags & 2u)) continue;
        if (!MethodCanHaveAotEntry(*old->second) || !MethodCanHaveAotEntry(method)) return false;
        plan.methods.emplace_back(old->second->token, method.token);
    }
    auto byBaseToken = [](const CurrentMetadataTokenBinding& left, const CurrentMetadataTokenBinding& right) {
        return left.baseToken < right.baseToken;
    };
    std::sort(plan.types.begin(), plan.types.end(), byBaseToken);
    std::sort(plan.methods.begin(), plan.methods.end(), byBaseToken);
    result = std::move(plan);
    return true;
}

static bool SameStableBaseAbiType(const Il2CppType* baseType, const Il2CppType* currentType)
{
    if (!baseType || !currentType || baseType->byref || currentType->byref ||
        baseType->type != currentType->type)
        return false;
    switch (baseType->type)
    {
    case IL2CPP_TYPE_VOID: case IL2CPP_TYPE_BOOLEAN: case IL2CPP_TYPE_CHAR:
    case IL2CPP_TYPE_I1: case IL2CPP_TYPE_U1: case IL2CPP_TYPE_I2: case IL2CPP_TYPE_U2:
    case IL2CPP_TYPE_I4: case IL2CPP_TYPE_U4: case IL2CPP_TYPE_I8: case IL2CPP_TYPE_U8:
    case IL2CPP_TYPE_R4: case IL2CPP_TYPE_R8: case IL2CPP_TYPE_I: case IL2CPP_TYPE_U:
    case IL2CPP_TYPE_STRING: case IL2CPP_TYPE_OBJECT:
        return true;
    default:
        // Value types, byrefs and generic contexts require the prepared Current
        // route. Matching names or sizes alone does not establish ABI identity.
        return false;
    }
}

static bool HasCompatibleStaticBaseFrame(const MethodInfo* baseMethod, const MethodInfo* currentMethod)
{
    if (!(baseMethod->flags & METHOD_ATTRIBUTE_STATIC) || !(currentMethod->flags & METHOD_ATTRIBUTE_STATIC) ||
        baseMethod->is_generic || currentMethod->is_generic || baseMethod->is_inflated || currentMethod->is_inflated ||
        baseMethod->klass->genericContainerHandle || currentMethod->klass->genericContainerHandle ||
        baseMethod->parameters_count != currentMethod->parameters_count ||
        !SameStableBaseAbiType(baseMethod->return_type, currentMethod->return_type))
        return false;
    for (uint8_t index = 0; index < baseMethod->parameters_count; ++index)
    {
#if HYBRIDCLR_UNITY_2021
        const Il2CppType* baseType = baseMethod->parameters[index].parameter_type;
        const Il2CppType* currentType = currentMethod->parameters[index].parameter_type;
#else
        const Il2CppType* baseType = baseMethod->parameters[index];
        const Il2CppType* currentType = currentMethod->parameters[index];
#endif
        if (!SameStableBaseAbiType(baseType, currentType))
            return false;
    }
    return true;
}

struct PendingMetaVersionRegistration
{
    const Il2CppAssembly* baseAssembly = nullptr;
    DHEAssemblyState state;
    std::vector<const MethodInfo*> methodsToPrepare;
    std::vector<uint32_t> preparedMethodTokens;
};

bool PrepareAndRegisterMetaVersions(
    const std::vector<MetaVersionRegistration>& registrations,
    const std::vector<Il2CppAssembly*>& interpreterAssemblies)
{
    std::lock_guard<std::recursive_mutex> lock(s_registrationMutex);
    // Keep the established registration -> metadata lock order. New assembly
    // name lookups take the metadata lock, so they see the complete committed
    // graph after the DHE release publication below.
    std::unique_ptr<il2cpp::os::FastAutoLock> metadataCommitLock;
    if (!interpreterAssemblies.empty())
    {
        metadataCommitLock.reset(new il2cpp::os::FastAutoLock(&il2cpp::vm::g_MetadataLock));
        for (Il2CppAssembly* assembly : interpreterAssemblies)
            if (!assembly || il2cpp::vm::MetadataCache::GetAssemblyByName(assembly->aname.name)) return false;
    }
    if (registrations.empty())
    {
        return false;
    }
    const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
    std::unordered_set<const Il2CppAssembly*> uniqueAssemblies;
    std::unordered_set<std::string> uniqueAssemblyNames;
    std::vector<PendingMetaVersionRegistration> pending;
    pending.reserve(registrations.size());

    // Resolve every Base method before any MethodInfo is modified.
    for (const MetaVersionRegistration& registration : registrations)
    {
        if (!registration.baseAssembly || !registration.baseMetaVersion ||
            !registration.currentMetaVersion || !registration.baseAssembly->aname.name)
        {
            return false;
        }
        const MetaVersionData& baseMetaVersion = *registration.baseMetaVersion;
        const MetaVersionData& currentMetaVersion = *registration.currentMetaVersion;
        std::vector<uint32_t> executionTokens;
        for (const CurrentMethodExecution& execution : registration.currentExecutions)
        {
            if (!execution.currentMethod) return false;
            executionTokens.push_back(execution.currentMethod->token);
        }
        // Validate before resolving/preparing any method, including direct
        // internal registrations which did not pass through image loading.
        CurrentImagePlan validatedSource;
        if (!ValidateCurrentImageSource(registration.source, baseMetaVersion.assemblyHash, currentMetaVersion.assemblyHash) ||
            (registration.source.kind == CurrentImageSourceKind::FrozenBaseAot &&
             !BuildCurrentImagePlan(baseMetaVersion, currentMetaVersion, {}, executionTokens, validatedSource, registration.source)))
            return false;
        if (baseMetaVersion.assemblyName != currentMetaVersion.assemblyName ||
            baseMetaVersion.assemblyName != registration.baseAssembly->aname.name ||
            !uniqueAssemblies.insert(registration.baseAssembly).second ||
            !uniqueAssemblyNames.insert(baseMetaVersion.assemblyName).second ||
            published->assemblyStates.find(registration.baseAssembly) !=
                published->assemblyStates.end())
        {
            return false;
        }

        std::unordered_map<std::string, const MetaVersionMethod*> currentMethods;
        currentMethods.reserve(currentMetaVersion.methods.size());
        for (const MetaVersionMethod& method : currentMetaVersion.methods)
        {
            if (!currentMethods.emplace(DigestKey(method.stableId), &method).second)
            {
                return false;
            }
        }

        std::unordered_set<std::string> currentTypes;
        currentTypes.reserve(currentMetaVersion.types.size());
        for (const MetaVersionType& type : currentMetaVersion.types)
        {
            if (!currentTypes.insert(DigestKey(type.stableId)).second)
            {
                return false;
            }
        }

        PendingMetaVersionRegistration plan;
        plan.baseAssembly = registration.baseAssembly;
        plan.state.source = registration.source;
        std::unordered_map<uint32_t, const MethodInfo*> currentExecutions;
        for (const CurrentMethodExecution& execution : registration.currentExecutions)
        {
            const MethodInfo* method = execution.currentMethod;
            if (!method || !method->klass || !method->klass->image || method->is_inflated ||
                method->klass->image->assembly != registration.baseAssembly ||
                method->klass->image == registration.baseAssembly->image ||
                !currentExecutions.emplace(execution.baseMethodToken, method).second)
                return false;
        }

        auto logicalMappings = s_logicalMethodMappings.find(registration.baseAssembly);
        if (logicalMappings != s_logicalMethodMappings.end())
        {
            plan.state.methodBaseTokens.reserve(logicalMappings->second.size() * 2);
            for (const auto& mapping : logicalMappings->second)
            {
                if (!mapping.first || !mapping.second || mapping.second->token == 0)
                {
                    return false;
                }
                const uint32_t baseToken = mapping.second->token;
                auto currentIdentity = plan.state.methodBaseTokens.emplace(
                    mapping.first, baseToken);
                if (!currentIdentity.second && currentIdentity.first->second != baseToken)
                {
                    return false;
                }
                auto baseIdentity = plan.state.methodBaseTokens.emplace(
                    mapping.second, baseToken);
                if (!baseIdentity.second && baseIdentity.first->second != baseToken)
                {
                    return false;
                }
            }
        }
        for (const MetaVersionType& baseType : baseMetaVersion.types)
        {
            if (currentTypes.find(DigestKey(baseType.stableId)) == currentTypes.end())
            {
                plan.state.removedTypeTokens.insert(baseType.token);
            }
        }
        for (const MetaVersionMethod& baseMethodVersion : baseMetaVersion.methods)
        {
            auto currentVersionEntry = currentMethods.find(
                DigestKey(baseMethodVersion.stableId));
            const MetaVersionMethod* currentMethodVersion =
                currentVersionEntry == currentMethods.end() ? nullptr :
                    currentVersionEntry->second;
            auto execution = currentExecutions.find(baseMethodVersion.token);
            const MethodInfo* currentExecution = execution == currentExecutions.end() ? nullptr : execution->second;
            if (currentExecution && (!currentMethodVersion || !MethodCanHaveAotEntry(baseMethodVersion) ||
                !MethodCanHaveAotEntry(*currentMethodVersion) || currentExecution->token != currentMethodVersion->token))
                return false;
            const bool conditional = std::binary_search(registration.source.genericContextMethodTokens.begin(),
                registration.source.genericContextMethodTokens.end(), baseMethodVersion.token);
            if (conditional && (!currentExecution ||
                (!currentExecution->is_generic && !currentExecution->klass->genericContainerHandle)))
                return false;
            const bool changed = !currentMethodVersion ||
                baseMethodVersion.version != currentMethodVersion->version || currentExecution;
            if (!MethodCanHaveAotEntry(baseMethodVersion) || !changed)
            {
                continue;
            }
            if (currentMethodVersion && !MethodCanHaveAotEntry(*currentMethodVersion))
            {
                return false;
            }
            const MethodInfo* baseMethod = ResolveMethodInAssembly(
                registration.baseAssembly, baseMethodVersion.token);
            if (!baseMethod)
            {
                return false;
            }
            plan.state.changedMethodTokens.insert(baseMethodVersion.token);
            plan.state.baseMethods.emplace(baseMethodVersion.token, baseMethod);
            auto baseIdentity = plan.state.methodBaseTokens.emplace(
                baseMethod, baseMethodVersion.token);
            if (!baseIdentity.second && baseIdentity.first->second != baseMethodVersion.token)
            {
                return false;
            }
            if (!currentMethodVersion)
            {
                plan.state.resolvedMethods.emplace(baseMethodVersion.token, nullptr);
            }
            else
            {
                if (currentExecution)
                {
                    auto currentIdentity = plan.state.methodBaseTokens.emplace(currentExecution, baseMethodVersion.token);
                    if (!currentIdentity.second && currentIdentity.first->second != baseMethodVersion.token)
                        return false;
                    if (!HasCompatibleStaticBaseFrame(baseMethod, currentExecution))
                        plan.state.incompatibleBaseAbiTokens.insert(baseMethodVersion.token);
                    currentExecutions.erase(execution);
                }
                plan.methodsToPrepare.push_back(currentExecution ? currentExecution : baseMethod);
                plan.preparedMethodTokens.push_back(baseMethodVersion.token);
            }
        }
        if (!currentExecutions.empty())
            return false;
        pending.push_back(std::move(plan));
    }

    std::vector<MethodPreparationSnapshot> snapshots;
    std::unique_ptr<PublishedState> next;
    try
    {
        for (PendingMetaVersionRegistration& plan : pending)
        {
            snapshots.reserve(snapshots.size() + plan.methodsToPrepare.size());
            if (!PrepareResolvedMethods(plan.methodsToPrepare, snapshots))
            {
                RollbackMethodPreparations(snapshots);
                return false;
            }
            for (size_t index = 0; index < plan.methodsToPrepare.size(); ++index)
                plan.state.resolvedMethods.emplace(plan.preparedMethodTokens[index],
                    plan.methodsToPrepare[index]);
        }
        // Finish allocations before committing vtables. Signature resolution
        // and state allocation can throw as well as return a failure code.
        next.reset(new PublishedState(*published));
        for (PendingMetaVersionRegistration& plan : pending)
            next->assemblyStates.emplace(plan.baseAssembly, std::move(plan.state));
    }
    catch (...)
    {
        RollbackMethodPreparations(snapshots);
        throw;
    }

    CommitMethodPreparations(snapshots);
    for (Il2CppAssembly* assembly : interpreterAssemblies)
        il2cpp::vm::MetadataCache::RegisterInterpreterAssembly(assembly);
    s_publishedState.store(next.release(), std::memory_order_release);
    return true;
}

bool PrepareAndRegisterMetaVersion(const Il2CppAssembly* baseAssembly,
    const MetaVersionData& baseMetaVersion, const MetaVersionData& currentMetaVersion)
{
    return PrepareAndRegisterMetaVersions({
        { baseAssembly, &baseMetaVersion, &currentMetaVersion }
    });
}

int32_t ExecuteInterpreterI4I4(const MethodInfo* method, int32_t value)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[1] = {};
    args[0].i32 = value;
    int32_t result = 0;
    hybridclr::interpreter::Interpreter::Execute(method, args, &result);
    return result;
}

int32_t ExecuteInterpreterI4I4I4(const MethodInfo* method, int32_t left, int32_t right)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[2] = {};
    args[0].i32 = left;
    args[1].i32 = right;
    int32_t result = 0;
    hybridclr::interpreter::Interpreter::Execute(method, args, &result);
    return result;
}

int32_t ExecuteInterpreterPtrI4(const MethodInfo* method, void* pointer, int32_t value)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[2] = {};
    args[0].ptr = pointer;
    args[1].i32 = value;
    int32_t result = 0;
    hybridclr::interpreter::Interpreter::Execute(method, args, &result);
    return result;
}

int64_t ExecuteInterpreterI8I8(const MethodInfo* method, int64_t value)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[1] = {};
    args[0].i64 = value;
    int64_t result = 0;
    hybridclr::interpreter::Interpreter::Execute(method, args, &result);
    return result;
}

void ExecuteInterpreterVoidI4(const MethodInfo* method, int32_t value)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[1] = {};
    args[0].i32 = value;
    hybridclr::interpreter::Interpreter::Execute(method, args, nullptr);
}

void ExecuteInterpreterVoidNoArgs(const MethodInfo* method)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[1] = {};
    hybridclr::interpreter::Interpreter::Execute(method, args, nullptr);
}

void ExecuteInterpreterInstanceVoidNoArgs(const MethodInfo* method, void* thisPtr)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[1] = {};
    args[0].obj = reinterpret_cast<Il2CppObject*>(thisPtr);
    hybridclr::interpreter::Interpreter::Execute(method, args, nullptr);
}

void ExecuteInterpreterValueTypeInstanceVoidNoArgs(const MethodInfo* method, void* thisPtr)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[1] = {};
    // A value-type receiver is passed by address, not as an Il2CppObject*.
    args[0].ptr = thisPtr;
    hybridclr::interpreter::Interpreter::Execute(method, args, nullptr);
}

bool ExecuteInterpreterInstanceBool(const MethodInfo* method, void* thisPtr)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[1] = {};
    args[0].obj = reinterpret_cast<Il2CppObject*>(thisPtr);
    int32_t result = 0;
    hybridclr::interpreter::Interpreter::Execute(method, args, &result);
    return result != 0;
}

void ExecuteInterpreterRefValueI4Ref(const MethodInfo* method, void* valuePtr, void* resultPtr)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[2] = {};
    args[0].ptr = valuePtr;
    args[1].ptr = resultPtr;
    hybridclr::interpreter::Interpreter::Execute(method, args, nullptr);
}

void ExecuteInterpreterValue(const MethodInfo* method, const void* value, uint32_t valueSize, void* result)
{
    RequireDheInterpreterMethod(method);
    if (!value || !result || valueSize == 0)
    {
        il2cpp::vm::Exception::Raise(
            il2cpp::vm::Exception::GetExecutionEngineException(
                "DHE value bridge received invalid storage"));
    }
    const uint32_t stackObjectCount = (valueSize + sizeof(hybridclr::interpreter::StackObject) - 1) /
        sizeof(hybridclr::interpreter::StackObject);
    std::vector<hybridclr::interpreter::StackObject> args(stackObjectCount);
    std::memcpy(args.data(), value, valueSize);
    hybridclr::interpreter::Interpreter::Execute(method, args.data(), result);
}

void ExecuteInterpreterInvokeArgs(const MethodInfo* method, void* thisPtr,
    void** argumentValues, const uint8_t* argumentKinds, uint32_t argumentCount, void* result)
{
    RequireDheInterpreterMethod(method);
    if (argumentCount != method->parameters_count ||
        (argumentCount != 0 && (!argumentValues || !argumentKinds)))
    {
        il2cpp::vm::Exception::Raise(
            il2cpp::vm::Exception::GetExecutionEngineException(
                "DHE invoke-args bridge received an invalid argument contract"));
    }

    auto* interpMethod = static_cast<hybridclr::interpreter::InterpMethodInfo*>(method->interpData);
    if (!interpMethod)
    {
        il2cpp::vm::Exception::Raise(
            il2cpp::vm::Exception::GetExecutionEngineException(
                "DHE invoke-args bridge has no interpreter method"));
    }

    const bool isInstance = (method->flags & METHOD_ATTRIBUTE_STATIC) == 0;
    if (isInstance && !thisPtr)
    {
        il2cpp::vm::Exception::Raise(
            il2cpp::vm::Exception::GetExecutionEngineException(
                "DHE invoke-args bridge has no instance receiver"));
    }

    std::vector<hybridclr::interpreter::StackObject> stackArguments(interpMethod->argStackObjectSize);
    uint32_t stackOffset = 0;
    if (isInstance)
    {
        stackArguments[0].ptr = thisPtr;
        stackOffset = 1;
    }
    uint32_t destinationIndex = stackOffset;
    auto* argumentDescriptions = interpMethod->args + stackOffset;
    for (uint32_t index = 0; index < argumentCount; ++index)
    {
        auto& description = argumentDescriptions[index];
        auto* destination = stackArguments.data() + destinationIndex;
        if (argumentKinds[index] > 1u ||
            (argumentKinds[index] == 0u && description.passbyValWhenInvoke) ||
            (!description.passbyValWhenInvoke && !argumentValues[index]))
        {
            il2cpp::vm::Exception::Raise(
                il2cpp::vm::Exception::GetExecutionEngineException(
                    "DHE invoke-args bridge received an incompatible generated argument"));
        }
        if (description.passbyValWhenInvoke)
        {
            // Reference arguments use their raw object pointer, including a
            // legal null. FGS values also arrive as raw invoke values; their
            // actual MethodArgDesc selects this path only for references.
            destination->ptr = argumentValues[index];
            ++destinationIndex;
        }
        else
        {
            // Concrete values use an address supplied by the generated guard;
            // FGS values already are pointers to their value storage.
            std::memcpy(destination, argumentValues[index],
                description.stackObjectSize * sizeof(hybridclr::interpreter::StackObject));
            destinationIndex += description.stackObjectSize;
        }
    }
    hybridclr::interpreter::Interpreter::Execute(method, stackArguments.data(), result);
}

int32_t ExecuteInterpreterInstanceI4I4(const MethodInfo* method, void* thisPtr, int32_t value)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[2] = {};
    args[0].obj = reinterpret_cast<Il2CppObject*>(thisPtr);
    args[1].i32 = value;
    int32_t result = 0;
    hybridclr::interpreter::Interpreter::Execute(method, args, &result);
    return result;
}

int64_t ExecuteInterpreterInstanceI8I8(const MethodInfo* method, void* thisPtr, int64_t value)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[2] = {};
    args[0].obj = reinterpret_cast<Il2CppObject*>(thisPtr);
    args[1].i64 = value;
    int64_t result = 0;
    hybridclr::interpreter::Interpreter::Execute(method, args, &result);
    return result;
}

void ExecuteInterpreterInstanceVoidI4(const MethodInfo* method, void* thisPtr, int32_t value)
{
    RequireDheInterpreterMethod(method);
    hybridclr::interpreter::StackObject args[2] = {};
    args[0].obj = reinterpret_cast<Il2CppObject*>(thisPtr);
    args[1].i32 = value;
    hybridclr::interpreter::Interpreter::Execute(method, args, nullptr);
}

void RecordInterpreterEntry()
{
    s_interpreterEntryCount.fetch_add(1, std::memory_order_relaxed);
}

void RecordAotBridgeCall()
{
    s_aotBridgeCallCount.fetch_add(1, std::memory_order_relaxed);
}

void RecordAotEntry()
{
    s_aotEntryCount.fetch_add(1, std::memory_order_relaxed);
}

int32_t GetInterpreterEntryCount()
{
    return s_interpreterEntryCount.load(std::memory_order_relaxed);
}

int32_t GetAotBridgeCallCount()
{
    return s_aotBridgeCallCount.load(std::memory_order_relaxed);
}

int32_t GetAotEntryCount()
{
    return s_aotEntryCount.load(std::memory_order_relaxed);
}

void ResetDispatchCounters()
{
    s_interpreterEntryCount.store(0, std::memory_order_relaxed);
    s_aotBridgeCallCount.store(0, std::memory_order_relaxed);
    s_aotEntryCount.store(0, std::memory_order_relaxed);
}

void ResetForTests()
{
    std::lock_guard<std::recursive_mutex> lock(s_registrationMutex);
    s_publishedState.store(new PublishedState(), std::memory_order_release);
    s_logicalMethodMappings.clear();
    ResetDispatchCounters();
}
}
}
