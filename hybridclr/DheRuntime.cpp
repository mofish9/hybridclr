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
#include "vm/Class.h"
#include "vm/Image.h"
#include "interpreter/Interpreter.h"
#include "interpreter/InterpreterModule.h"
#include "interpreter/InterpreterDefs.h"
#include "vm/Exception.h"
#include "Il2CppCompatibleDef.h"

namespace hybridclr::dhe
{
namespace
{
    constexpr char kMetaVersionMagic[] = "DHEMVLT1";
    constexpr size_t kMetaVersionMagicSize = sizeof(kMetaVersionMagic) - 1;
    constexpr size_t kMetaVersionFixedHeaderSize = kMetaVersionMagicSize + 4 * sizeof(uint32_t) + 64;

    struct DHEAssemblyState
    {
        std::unordered_set<uint32_t> changedMethodTokens;
        std::unordered_map<uint32_t, const MethodInfo*> resolvedMethods;
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
    uint32_t methodCount = 0;
    if (!ReadU32(bytes, size, offset, schemaVersion) ||
        !ReadU32(bytes, size, offset, assemblyNameSize) ||
        !ReadU32(bytes, size, offset, methodCount) ||
        !ReadU32(bytes, size, offset, result.flags))
    {
        return false;
    }
    if (schemaVersion != kMetaVersionSchema || assemblyNameSize == 0 || assemblyNameSize > 1024 ||
        (result.flags & ~kMetaVersionKnownFlags) != 0)
    {
        return false;
    }

    // The first format stores 32 bytes each for baseline/current SHA-256.
    if (size - offset < 64 || size - offset - 64 < assemblyNameSize)
    {
        return false;
    }
    std::memcpy(result.baselineAssemblyHash.data(), bytes + offset, kSha256DigestSize);
    std::memcpy(result.currentAssemblyHash.data(), bytes + offset + kSha256DigestSize, kSha256DigestSize);
    offset += 64;

    if (size - offset < assemblyNameSize)
    {
        return false;
    }

    const size_t tokenBytes = size - offset - assemblyNameSize;
    if (methodCount > tokenBytes / sizeof(uint32_t) ||
        tokenBytes != static_cast<size_t>(methodCount) * sizeof(uint32_t))
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

    result.changedMethodTokens.reserve(methodCount);
    std::unordered_set<uint32_t> uniqueTokens;
    for (uint32_t i = 0; i < methodCount; ++i)
    {
        uint32_t token = 0;
        if (!ReadU32(bytes, size, offset, token) || token == 0 || !uniqueTokens.insert(token).second)
        {
            result = MetaVersionData{};
            return false;
        }
        result.changedMethodTokens.push_back(token);
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

bool RegisterChangedMethods(const Il2CppAssembly* assembly,
    const std::vector<uint32_t>& changedMethodTokens,
    const std::vector<const MethodInfo*>& resolvedMethods)
{
    if (!assembly)
    {
        return false;
    }
    if (resolvedMethods.size() != changedMethodTokens.size())
    {
        return false;
    }

    DHEAssemblyState state;
    state.changedMethodTokens.reserve(changedMethodTokens.size());
    state.resolvedMethods.reserve(resolvedMethods.size());
    for (size_t index = 0; index < changedMethodTokens.size(); ++index)
    {
        const uint32_t token = changedMethodTokens[index];
        if (token == 0)
        {
            return false;
        }
        if (!state.changedMethodTokens.insert(token).second || resolvedMethods[index] == nullptr)
        {
            return false;
        }
        state.resolvedMethods.emplace(token, resolvedMethods[index]);
    }

    std::lock_guard<std::recursive_mutex> lock(s_registrationMutex);
    const PublishedState* current = s_publishedState.load(std::memory_order_acquire);
    if (current->assemblyStates.find(assembly) != current->assemblyStates.end())
    {
        return false;
    }
    std::unique_ptr<PublishedState> next(new PublishedState(*current));
    next->assemblyStates.emplace(assembly, std::move(state));
    s_publishedState.store(next.release(), std::memory_order_release);
    return true;
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
    return state->second.changedMethodTokens.find(method->token) !=
        state->second.changedMethodTokens.end();
}

bool ShouldDispatchToInterpreter(const MethodInfo* method)
{
    // Keep the selector cheap; registration resolves all changed methods once,
    // while preparation is completed on the first helper call.
    return IsChangedMethod(method);
}

const MethodInfo* ResolveMethodByToken(const char* assemblyName, uint32_t token)
{
    if (!assemblyName || assemblyName[0] == '\0' || token == 0)
    {
        return nullptr;
    }

    const Il2CppAssembly* assembly = il2cpp::vm::MetadataCache::GetAssemblyByName(assemblyName);
    if (!assembly)
    {
        return nullptr;
    }
    const PublishedState* published = s_publishedState.load(std::memory_order_acquire);
    auto assemblyState = published->assemblyStates.find(assembly);
    if (assemblyState != published->assemblyStates.end())
    {
        auto resolved = assemblyState->second.resolvedMethods.find(token);
        if (resolved != assemblyState->second.resolvedMethods.end())
        {
            return resolved->second;
        }
    }
    const Il2CppImage* image = il2cpp::vm::Assembly::GetImage(assembly);
    if (!image)
    {
        return nullptr;
    }

    il2cpp::vm::TypeVector types;
    il2cpp::vm::Image::GetTypes(image, false, &types);
    for (const Il2CppClass* type : types)
    {
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

bool PrepareChangedMethods(const Il2CppAssembly* assembly,
    const std::vector<uint32_t>& changedMethodTokens,
    std::vector<const MethodInfo*>& resolvedMethods)
{
    // Preparation mutates MethodInfo and vtable state. Serialize it with the
    // registration commit so a concurrent failed load cannot restore a
    // snapshot over a successful load's published interpreter pointers.
    std::lock_guard<std::recursive_mutex> lock(s_registrationMutex);
    if (!assembly || !assembly->aname.name)
    {
        return false;
    }

    if (IsDheAssembly(assembly))
    {
        return false;
    }

    resolvedMethods.clear();
    resolvedMethods.reserve(changedMethodTokens.size());
    std::vector<MethodPreparationSnapshot> snapshots;
    snapshots.reserve(changedMethodTokens.size());
    const auto rollback = [&snapshots]()
    {
        for (auto it = snapshots.rbegin(); it != snapshots.rend(); ++it)
        {
            RestoreMethodPreparationSnapshot(*it);
        }
    };
    for (uint32_t token : changedMethodTokens)
    {
        const MethodInfo* method = ResolveMethodByToken(assembly->aname.name, token);
        resolvedMethods.push_back(method);
        if (!method)
        {
            rollback();
            resolvedMethods.clear();
            return false;
        }

        // A MV token identifies a method definition. Generic method
        // definitions and methods declared on generic types do not have a
        // concrete direct-call ABI until an inflated instantiation is used;
        // the generated-code resolver excludes those shapes. Leave them to
        // the normal interpreter metadata path instead of rejecting an
        // otherwise valid partial DHE image at load time.
        if (method->is_generic ||
            (method->klass && (method->klass->generic_class || method->klass->genericContainerHandle)))
        {
            continue;
        }

        snapshots.push_back(CaptureMethodPreparationSnapshot(method));
        Il2CppMethodPointer interpreterPointer =
            hybridclr::InitAndGetInterpreterDirectlyCallMethodPointer(method);
        if (!interpreterPointer)
        {
            rollback();
            resolvedMethods.clear();
            return false;
        }
        // The direct-call pointer can have been initialized by an earlier
        // interpreter bridge. DHE still owns the dispatch decision for this
        // token, so make the interpreter implementation bit explicit.
        const_cast<MethodInfo*>(method)->isInterpterImpl = true;
    }

    // Publish all virtual slots only after every changed method has a valid
    // interpreter entry. This is the commit point of preparation.
    for (const MethodPreparationSnapshot& snapshot : snapshots)
    {
        if (snapshot.vtableEntry)
        {
            snapshot.vtableEntry->method = snapshot.method;
            snapshot.vtableEntry->methodPtr =
                snapshot.method->virtualMethodPointerCallByInterp;
        }
    }
    return true;
}

bool PrepareAndRegisterChangedMethods(const Il2CppAssembly* assembly,
    const std::vector<uint32_t>& changedMethodTokens)
{
    std::lock_guard<std::recursive_mutex> lock(s_registrationMutex);
    std::vector<const MethodInfo*> resolvedMethods;
    if (!PrepareChangedMethods(assembly, changedMethodTokens, resolvedMethods))
    {
        return false;
    }
    return RegisterChangedMethods(assembly, changedMethodTokens, resolvedMethods);
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
    ResetDispatchCounters();
}
}
