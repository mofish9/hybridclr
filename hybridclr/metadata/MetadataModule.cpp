#include "MetadataModule.h"
#include "DheCustomAttributeMetadata.h"

#include "os/Atomic.h"
#include "os/Mutex.h"
#include "os/File.h"
#include "vm/Exception.h"
#include "vm/String.h"
#include "vm/Assembly.h"
#include "vm/Class.h"
#include "vm/Domain.h"
#include "vm/Object.h"
#include "vm/Array.h"
#include "vm/Field.h"
#include "vm/Image.h"
#include "vm/MetadataLock.h"
#include "vm/MetadataCache.h"
#include "vm/Method.h"
#include "gc/GarbageCollector.h"
#include "gc/GCHandle.h"
#include "gc/WriteBarrier.h"
#include "utils/Logging.h"
#include "utils/MemoryMappedFile.h"
#include "utils/Memory.h"

#include "../interpreter/InterpreterModule.h"

#include "Assembly.h"
#include "InterpreterImage.h"
#include "ConsistentAOTHomologousImage.h"
#include "SuperSetAOTHomologousImage.h"
#include "MetadataPool.h"

#include <mutex>
#include <unordered_map>
#include <vector>

using namespace il2cpp;

namespace hybridclr
{
	namespace dhe
	{
		bool TryGetVirtualInvokeData(const Il2CppClass* klass, uint16_t logicalSlot,
			const VirtualInvokeData*& result)
		{
			return metadata::MetadataModule::TryGetDheVirtualInvokeData(klass, logicalSlot, result);
		}

		bool TryGetVirtualInvokeData(const Il2CppClass* klass, const MethodInfo* method,
			const VirtualInvokeData*& result)
		{
			return metadata::MetadataModule::TryGetDheVirtualInvokeData(klass, method, result);
		}

		bool TryGetVirtualBaseMethod(const MethodInfo* method, bool definition, const MethodInfo*& result)
		{
			return metadata::MetadataModule::TryGetDheVirtualBaseMethod(method, definition, result);
		}

		bool TryGetInterfaceInvokeData(const Il2CppClass* klass, const Il2CppClass* interfaceType,
			uint16_t logicalSlot, const VirtualInvokeData*& result)
		{
			return metadata::MetadataModule::TryGetDheInterfaceInvokeData(klass, interfaceType, logicalSlot, result);
		}
	}

namespace metadata
{
	namespace
	{
		constexpr uint32_t kDheSidecarSlabCapacity = 256;

		struct DheEphemeron
		{
			Il2CppObject* key;
			Il2CppObject* value;
		};

		struct DheSidecarLocation
		{
			uint32_t slabIndex;
			uint32_t entryIndex;
		};

		struct DheSidecarSlab
		{
			uint32_t gcHandle;
			uint32_t used;
		};

		struct DheInstanceFieldSlot
		{
			uint32_t slot;
			FieldInfo* logicalField;
		};

		struct DheFieldCellLayout
		{
			Il2CppClass* klass;
			FieldInfo* value;
			int32_t ownerOffset;
		};

		// Cell layouts are immutable after publication under g_MetadataLock.
		std::unordered_map<Il2CppClass*, DheFieldCellLayout> s_dheFieldCellLayouts;
		// Node addresses remain stable across rehash; entries are immutable after insertion.
		std::unordered_map<const Il2CppClass*, std::unordered_map<const Il2CppClass*,
			std::unordered_map<uint16_t, VirtualInvokeData>>> s_dheInterfaceDispatch;
		std::unordered_map<const Il2CppClass*, std::unordered_map<uint16_t,
			VirtualInvokeData>> s_dheVirtualDispatch;
		std::unordered_map<const Il2CppClass*, std::unordered_map<const MethodInfo*,
			VirtualInvokeData>> s_dheVirtualMethodDispatch;

		std::mutex s_dheSidecarMutex;
		std::unordered_map<const FieldInfo*, DheInstanceFieldSlot> s_dheInstanceFieldSlots;
		uint32_t s_dheInstanceFieldSlotCount = 0;
		std::unordered_map<Il2CppObject*, DheSidecarLocation> s_dheObjectSidecars;
		std::vector<DheSidecarSlab> s_dheSidecarSlabs;

		Il2CppArray* GetDheSidecarSlabArray(const DheSidecarSlab& slab)
		{
			return reinterpret_cast<Il2CppArray*>(gc::GCHandle::GetTarget(slab.gcHandle));
		}

		DheEphemeron* GetDheSidecarEntry(const DheSidecarLocation& location)
		{
			if (location.slabIndex >= s_dheSidecarSlabs.size())
			{
				return nullptr;
			}
			Il2CppArray* array = GetDheSidecarSlabArray(
				s_dheSidecarSlabs[location.slabIndex]);
			if (!array || location.entryIndex >= array->max_length)
			{
				return nullptr;
			}
			return il2cpp_array_addr(array, DheEphemeron, location.entryIndex);
		}

		DheSidecarLocation AllocateDheSidecarEntryLocked()
		{
			for (uint32_t slabIndex = 0;
				slabIndex < static_cast<uint32_t>(s_dheSidecarSlabs.size()); ++slabIndex)
			{
				DheSidecarSlab& slab = s_dheSidecarSlabs[slabIndex];
				Il2CppArray* array = GetDheSidecarSlabArray(slab);
				if (slab.used < array->max_length)
				{
					return { slabIndex, slab.used++ };
				}
				for (uint32_t entryIndex = 0;
					entryIndex < static_cast<uint32_t>(array->max_length); ++entryIndex)
				{
					DheEphemeron* entry = il2cpp_array_addr(array, DheEphemeron, entryIndex);
					if (!entry->key || entry->key == il2cpp::vm::Domain::GetCurrent()->ephemeron_tombstone)
					{
						return { slabIndex, entryIndex };
					}
				}
			}

			Il2CppClass* ephemeronClass = il2cpp::vm::Class::FromName(il2cpp_defaults.corlib,
				"System.Runtime.CompilerServices", "Ephemeron");
			if (!ephemeronClass)
			{
				RaiseExecutionEngineException("DHE sidecar requires System.Runtime.CompilerServices.Ephemeron.");
			}
			Il2CppArray* array = il2cpp::vm::Array::New(ephemeronClass, kDheSidecarSlabCapacity);
			if (!gc::GarbageCollector::EphemeronArrayAdd(reinterpret_cast<Il2CppObject*>(array)))
			{
				RaiseExecutionEngineException("DHE sidecar ephemeron registration failed.");
			}
			uint32_t handle = gc::GCHandle::New(reinterpret_cast<Il2CppObject*>(array), false);
			if (!handle)
			{
				RaiseExecutionEngineException("DHE sidecar ephemeron root allocation failed.");
			}
			s_dheSidecarSlabs.push_back({ handle, 1 });
			return { static_cast<uint32_t>(s_dheSidecarSlabs.size() - 1), 0 };
		}

		DheEphemeron* GetOrCreateDheSidecarEntryLocked(Il2CppObject* obj)
		{
			auto existing = s_dheObjectSidecars.find(obj);
			if (existing != s_dheObjectSidecars.end())
			{
				DheEphemeron* entry = GetDheSidecarEntry(existing->second);
				if (entry && entry->key == obj)
				{
					return entry;
				}
				s_dheObjectSidecars.erase(existing);
			}

			DheSidecarLocation location = AllocateDheSidecarEntryLocked();
			DheEphemeron* entry = GetDheSidecarEntry(location);
			gc::WriteBarrier::GenericStore(&entry->key, obj);
			gc::WriteBarrier::GenericStoreNull(&entry->value);
			s_dheObjectSidecars[obj] = location;
			return entry;
		}

		Il2CppArray* EnsureDheSidecarValuesLocked(DheEphemeron* entry, uint32_t size)
		{
			Il2CppArray* values = reinterpret_cast<Il2CppArray*>(entry->value);
			if (values && values->max_length >= size)
			{
				return values;
			}

			uint32_t capacity = values ? static_cast<uint32_t>(values->max_length) : 0;
			capacity = capacity == 0 ? 4 : capacity;
			while (capacity < size)
			{
				if (capacity > UINT32_MAX / 2)
				{
					RaiseExecutionEngineException("DHE sidecar field capacity overflow.");
				}
				capacity *= 2;
			}
			Il2CppArray* expanded = il2cpp::vm::Array::New(il2cpp_defaults.object_class, capacity);
			if (values)
			{
				for (uint32_t index = 0;
					index < static_cast<uint32_t>(values->max_length); ++index)
				{
					Il2CppObject* value = il2cpp_array_get(values, Il2CppObject*, index);
					il2cpp_array_setref(expanded, index, value);
				}
			}
			gc::WriteBarrier::GenericStore(&entry->value,
				reinterpret_cast<Il2CppObject*>(expanded));
			return expanded;
		}

		bool TryGetDheFieldSlotLocked(const FieldInfo* field, uint32_t& slot,
			FieldInfo*& logicalField)
		{
			auto fieldSlot = s_dheInstanceFieldSlots.find(field);
			if (fieldSlot == s_dheInstanceFieldSlots.end())
			{
				return false;
			}
			slot = fieldSlot->second.slot;
			logicalField = fieldSlot->second.logicalField;
			return true;
		}

		Il2CppObject* GetDheSidecarValueLocked(Il2CppObject* obj, uint32_t slot)
		{
			auto sidecar = s_dheObjectSidecars.find(obj);
			if (sidecar == s_dheObjectSidecars.end())
			{
				return nullptr;
			}
			DheEphemeron* entry = GetDheSidecarEntry(sidecar->second);
			if (!entry || entry->key != obj)
			{
				s_dheObjectSidecars.erase(sidecar);
				return nullptr;
			}
			Il2CppArray* values = reinterpret_cast<Il2CppArray*>(entry->value);
			return values && slot < values->max_length
				? il2cpp_array_get(values, Il2CppObject*, slot) : nullptr;
		}

		void SetDheSidecarValueLocked(Il2CppObject* obj, uint32_t slot,
			Il2CppObject* value)
		{
			DheEphemeron* entry = GetOrCreateDheSidecarEntryLocked(obj);
			Il2CppArray* values = EnsureDheSidecarValuesLocked(entry, slot + 1);
			il2cpp_array_setref(values, slot, value);
		}

		DheFieldCellLayout GetDheFieldCellLayoutLocked(const Il2CppType* type)
		{
			Il2CppClass* fieldType = il2cpp::vm::Class::FromIl2CppType(type);
			auto existing = s_dheFieldCellLayouts.find(fieldType);
			if (existing != s_dheFieldCellLayouts.end())
				return existing->second;

			const Il2CppAssembly* assembly = il2cpp::vm::Assembly::GetLoadedAssembly("HybridCLR.Runtime");
			Il2CppClass* definition = assembly ? il2cpp::vm::Class::FromName(
				assembly->image, "HybridCLR", "DheFieldCell`1") : nullptr;
			if (!definition || !definition->is_generic)
				RaiseExecutionEngineException("DHE requires the preserved HybridCLR.DheFieldCell<T> runtime type.");
			const Il2CppType* argument = &fieldType->byval_arg;
			Il2CppClass* cellClass = il2cpp::vm::MetadataCache::GetGenericInstanceType(definition, &argument, 1);
			if (!cellClass)
				RaiseExecutionEngineException("DHE field cell generic instantiation failed.");
			il2cpp::vm::Class::Init(cellClass);
			FieldInfo* owner = il2cpp::vm::Class::GetFieldFromName(cellClass, "Owner");
			FieldInfo* value = il2cpp::vm::Class::GetFieldFromName(cellClass, "Value");
			if (!owner || !value || owner->type->type != IL2CPP_TYPE_OBJECT ||
				(owner->type->attrs & FIELD_ATTRIBUTE_STATIC) || (value->type->attrs & FIELD_ATTRIBUTE_STATIC) ||
				owner->offset < static_cast<int32_t>(sizeof(Il2CppObject)) ||
				value->offset < static_cast<int32_t>(sizeof(Il2CppObject)) ||
				il2cpp::vm::Class::FromIl2CppType(value->type) != fieldType)
				RaiseExecutionEngineException("DHE field cell layout is missing or incompatible.");
			uint32_t valueSize = fieldType->byval_arg.valuetype
				? il2cpp::vm::Class::GetValueSize(fieldType, nullptr) : sizeof(Il2CppObject*);
			uint64_t ownerEnd = static_cast<uint64_t>(owner->offset) + sizeof(Il2CppObject*);
			uint64_t valueEnd = static_cast<uint64_t>(value->offset) + valueSize;
			if (ownerEnd > cellClass->instance_size || valueEnd > cellClass->instance_size ||
				(owner->offset < valueEnd && value->offset < ownerEnd))
				RaiseExecutionEngineException("DHE field cell storage overlaps or exceeds its managed layout.");
			DheFieldCellLayout layout = { cellClass, value, owner->offset };
			s_dheFieldCellLayouts.emplace(fieldType, layout);
			return layout;
		}

		bool TryGetDheFieldCell(Il2CppObject* obj, const FieldInfo* field,
			Il2CppObject*& cell, FieldInfo*& valueField)
		{
			uint32_t slot;
			FieldInfo* logicalField;
			{
				std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
				if (!TryGetDheFieldSlotLocked(field, slot, logicalField))
					return false;
			}
			if (!obj)
				il2cpp::vm::Exception::RaiseNullReferenceException();
			// Never enter engine Field APIs under the sidecar mutex: they reenter DHE.
			il2cpp::os::FastAutoLock metadataLock(&il2cpp::vm::g_MetadataLock);
			DheFieldCellLayout layout = GetDheFieldCellLayoutLocked(logicalField->type);
			valueField = layout.value;
			{
				std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
				cell = GetDheSidecarValueLocked(obj, slot);
				if (cell)
					return true;
			}
			Il2CppObject* created = il2cpp::vm::Object::New(layout.klass);
			gc::WriteBarrier::GenericStore(reinterpret_cast<Il2CppObject**>(
				reinterpret_cast<uint8_t*>(created) + layout.ownerOffset), obj);
			{
				std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
				cell = GetDheSidecarValueLocked(obj, slot);
				if (!cell)
				{
					SetDheSidecarValueLocked(obj, slot, created);
					cell = created;
				}
			}
			return true;
		}
	}



    void MetadataModule::Initialize()
    {
        MetadataPool::Initialize();
        InterpreterImage::Initialize();
        Assembly::InitializePlaceHolderAssemblies();
    }

    Image* MetadataModule::GetUnderlyingInterpreterImage(const MethodInfo* methodInfo)
    {
        if (metadata::IsInterpreterMethod(methodInfo))
        {
            return hybridclr::metadata::MetadataModule::GetImage(methodInfo->klass);
        }
        AOTHomologousImage* homologous = AOTHomologousImage::FindImageByAssembly(
            methodInfo->klass->rank ? il2cpp_defaults.corlib->assembly :
                methodInfo->klass->image->assembly);
        if (!homologous)
        {
            return nullptr;
        }
        Image* supplemental = homologous->GetSupplementalMethodImage(methodInfo);
        return supplemental ? supplemental : homologous;
    }

	Image* MetadataModule::GetInterpreterResolveImage(const MethodInfo* methodInfo)
	{
		Image* bodyImage = GetUnderlyingInterpreterImage(methodInfo);
		if (!methodInfo || !methodInfo->klass || !methodInfo->klass->image ||
			!methodInfo->klass->image->assembly)
		{
			return bodyImage;
		}
		AOTHomologousImage* homologous = AOTHomologousImage::FindImageByAssembly(
			methodInfo->klass->image->assembly);
		Image* resolveImage = homologous
			? homologous->GetMethodResolveImage(methodInfo) : nullptr;
		return resolveImage ? resolveImage : bodyImage;
	}

    static AOTHomologousImage* GetDheSupplementalImage(const Il2CppImage* image)
    {
        if (!image || !image->assembly || !dhe::IsDheAssembly(image->assembly))
        {
            return nullptr;
        }
        AOTHomologousImage* homologous = AOTHomologousImage::FindImageByAssembly(image->assembly);
        return homologous && homologous->GetTargetAssembly()->image == image ? homologous : nullptr;
    }

	static bool HasDheVirtualHierarchy(const Il2CppClass* klass)
	{
		if (!klass || klass->is_import_or_windows_runtime)
			return false;
		for (const Il2CppClass* owner = klass; owner; owner = owner->parent)
			if (owner->image && dhe::IsDheAssembly(owner->image->assembly))
				return true; // acquire the completed registration before accessing Current metadata
		return false;
	}

	static void InitDheVTable(Il2CppClass* klass)
	{
		il2cpp::vm::Class::Init(klass);
#if UNITY_ENGINE_TUANJIE
		il2cpp::vm::Class::SetupVTable(klass);
#endif
	}

	static Il2CppClass* GetDheCurrentClass(Il2CppClass* klass)
	{
		if (!klass)
			return nullptr;
		AOTHomologousImage* image = GetDheSupplementalImage(klass->image);
		const Il2CppType* type = image ? image->GetDheCurrentType(&klass->byval_arg) : nullptr;
		return type ? il2cpp::vm::Class::FromIl2CppType(type) : klass;
	}

	static const MethodInfo* DheLogicalDefinition(const MethodInfo* method)
	{
		method = MetadataModule::ResolveDheMethod(method);
		return method && method->is_inflated && method->genericMethod
			? method->genericMethod->methodDefinition : method;
	}

	static const MethodInfo* GetDheBaseVirtualRoot(Il2CppClass* klass, uint16_t slot)
	{
		InitDheVTable(klass);
		if (slot >= klass->vtable_count)
			RaiseExecutionEngineException("DHE Base virtual slot exceeds its native vtable.");
		while (klass->parent)
		{
			InitDheVTable(klass->parent);
			if (slot >= klass->parent->vtable_count)
				break;
			klass = klass->parent;
		}
		return klass->vtable[slot].method;
	}

	static const MethodInfo* FindDheCurrentVirtualDeclaration(const MethodInfo* method)
	{
		if (!method || !method->klass)
			RaiseExecutionEngineException("DHE virtual declaration is missing.");
		Il2CppClass* currentOwner = GetDheCurrentClass(method->klass);
		InitDheVTable(currentOwner);
		const MethodInfo* identity = DheLogicalDefinition(method);
		for (uint16_t slot = 0; slot < currentOwner->vtable_count; ++slot)
		{
			const MethodInfo* candidate = currentOwner->vtable[slot].method;
			if (candidate && DheLogicalDefinition(candidate) == identity)
				return candidate;
		}
		il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetMissingMethodException(
			"The Current type has no matching virtual declaration."));
		return nullptr;
	}

	static const MethodInfo* GetDheVirtualDeclaration(const MethodInfo* method)
	{
		// Base methods carry native slots; Current-only aliases carry Current
		// slots. Never infer which domain a number belongs to from its range.
		AOTHomologousImage* image = GetDheSupplementalImage(method->klass->image);
		if (!IsInterpreterType(method->klass) && (!image || !image->GetSupplementalMethodImage(method)))
			method = GetDheBaseVirtualRoot(method->klass, method->slot);
		return FindDheCurrentVirtualDeclaration(method);
	}

	static VirtualInvokeData GetDheVirtualEntry(const MethodInfo* method)
	{
		const MethodInfo* target = MetadataModule::ResolveDheMethod(method);
		if (!target || IsAbstractMethod(target->flags))
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetMissingMethodException(
				"The Current receiver has no concrete virtual implementation."));
#if HYBRIDCLR_UNITY_2021
		Il2CppMethodPointer pointer = target->is_generic ? nullptr : il2cpp::vm::Method::GetVirtualCallMethodPointer(target);
#else
		Il2CppMethodPointer pointer = target->is_generic ? nullptr : ReadPublishedPointer(&const_cast<MethodInfo*>(target)->virtualMethodPointer);
#endif
		// Open generic definitions are resolved before IL2CPP applies the call's
		// method arguments, and do not have a callable entry at this stage.
		if (!pointer && !target->is_generic)
			pointer = InitAndGetInterpreterDirectlyCallVirtualMethodPointer(target);
		if (!pointer && !target->is_generic)
			RaiseExecutionEngineException("DHE virtual method has no callable entry.");
		VirtualInvokeData entry = {};
		entry.method = target;
		entry.methodPtr = pointer;
		return entry;
	}

	static VirtualInvokeData ResolveDheVirtualEntry(const Il2CppClass* klass, const MethodInfo* declaration)
	{
		Il2CppClass* receiver = GetDheCurrentClass(const_cast<Il2CppClass*>(klass));
		InitDheVTable(receiver);
		if (receiver == klass && !IsInterpreterType(receiver))
		{
			// An ordinary AOT descendant has no Current shadow of its own.
			// Preserve its native overrides, but resolve inherited implementations
			// through the nearest Current parent. Never index its Base table with
			// a Current slot, including for a newly added inherited declaration.
			const MethodInfo* logical = MetadataModule::ResolveDheMethod(declaration);
			AOTHomologousImage* image = GetDheSupplementalImage(logical->klass->image);
			if (!IsInterpreterType(logical->klass) && (!image || !image->GetSupplementalMethodImage(logical)) &&
				logical->slot < receiver->vtable_count)
			{
				const MethodInfo* nativeTarget = receiver->vtable[logical->slot].method;
				if (nativeTarget && !dhe::IsDheAssembly(nativeTarget->klass->image->assembly))
					return GetDheVirtualEntry(nativeTarget);
			}
			if (!receiver->parent)
				RaiseExecutionEngineException("DHE receiver has no Current virtual parent.");
			return ResolveDheVirtualEntry(receiver->parent, declaration);
		}
		if (declaration->slot >= receiver->vtable_count)
			RaiseExecutionEngineException("DHE Current virtual slot exceeds the receiver vtable.");
		return GetDheVirtualEntry(receiver->vtable[declaration->slot].method);
	}

	bool MetadataModule::TryGetDheVirtualInvokeData(const Il2CppClass* klass,
		uint16_t logicalSlot, const VirtualInvokeData*& result)
	{
		if (!HasDheVirtualHierarchy(klass))
			return false;
		Il2CppClass* nativeAncestor = const_cast<Il2CppClass*>(klass);
		while (nativeAncestor && IsInterpreterType(nativeAncestor))
			nativeAncestor = nativeAncestor->parent;
		if (!nativeAncestor)
			return false;
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		InitDheVTable(nativeAncestor);
		if (logicalSlot >= nativeAncestor->vtable_count)
			return false;
		auto& cache = s_dheVirtualDispatch[klass];
		auto cached = cache.find(logicalSlot);
		if (cached == cache.end())
		{
			const MethodInfo* root = GetDheBaseVirtualRoot(nativeAncestor, logicalSlot);
			VirtualInvokeData entry = ResolveDheVirtualEntry(klass, FindDheCurrentVirtualDeclaration(root));
			cached = cache.emplace(logicalSlot, entry).first;
		}
		// No pointer escapes before full construction. Node addresses survive
		// rehash, and both readers and writers hold the metadata lock.
		result = &cached->second;
		return true;
	}

	bool MetadataModule::TryGetDheVirtualInvokeData(const Il2CppClass* klass,
		const MethodInfo* method, const VirtualInvokeData*& result)
	{
		if (!method || !IsVirtualMethod(method->flags) || IsInterface(method->klass->flags) ||
			!HasDheVirtualHierarchy(klass))
			return false;
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		auto& cache = s_dheVirtualMethodDispatch[klass];
		auto cached = cache.find(method);
		if (cached == cache.end())
		{
			VirtualInvokeData entry = ResolveDheVirtualEntry(klass, GetDheVirtualDeclaration(method));
			cached = cache.emplace(method, entry).first;
		}
		result = &cached->second;
		return true;
	}

	bool MetadataModule::TryGetDheVirtualBaseMethod(const MethodInfo* method, bool definition,
		const MethodInfo*& result)
	{
		if (!method || !IsVirtualMethod(method->flags) || IsInterface(method->klass->flags) ||
			!HasDheVirtualHierarchy(method->klass))
			return false;
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		const MethodInfo* current = FindDheCurrentVirtualDeclaration(method);
		Il2CppClass* owner = GetDheCurrentClass(method->klass);
		result = method;
		if (current->flags & METHOD_ATTRIBUTE_NEW_SLOT)
			return true;
		for (Il2CppClass* parent = GetDheCurrentClass(owner->parent); parent;
			parent = GetDheCurrentClass(parent->parent))
		{
			InitDheVTable(parent);
			if (current->slot >= parent->vtable_count)
				break;
			const MethodInfo* inherited = parent->vtable[current->slot].method;
			if (!inherited)
				RaiseExecutionEngineException("DHE Current base virtual declaration is missing.");
			result = ResolveDheMethod(inherited);
			if (!definition)
				break;
		}
		return true;
	}

	bool MetadataModule::TryGetDheInterfaceInvokeData(const Il2CppClass* klass,
		const Il2CppClass* interfaceType, uint16_t logicalSlot, const VirtualInvokeData*& result)
	{
		if (!klass || !interfaceType || !interfaceType->image || klass->is_import_or_windows_runtime ||
			!dhe::IsDheAssembly(interfaceType->image->assembly))
			return false;
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		AOTHomologousImage* interfaceImage = GetDheSupplementalImage(interfaceType->image);
		const MethodInfo* currentInterfaceMethod = nullptr;
		if (!interfaceImage || !interfaceImage->TryGetDheCurrentInterfaceMethod(
			interfaceType, logicalSlot, currentInterfaceMethod))
			return false;
		auto receiverCache = s_dheInterfaceDispatch.find(klass);
		if (receiverCache != s_dheInterfaceDispatch.end())
		{
			auto interfaceCache = receiverCache->second.find(interfaceType);
			if (interfaceCache != receiverCache->second.end())
			{
				auto entry = interfaceCache->second.find(logicalSlot);
				if (entry != interfaceCache->second.end())
				{
					result = &entry->second;
					return true;
				}
			}
		}
		Il2CppClass* currentClass = const_cast<Il2CppClass*>(klass);
		AOTHomologousImage* receiverImage = dhe::IsDheAssembly(klass->image->assembly)
			? AOTHomologousImage::FindImageByAssembly(klass->image->assembly) : nullptr;
		const Il2CppType* currentType = receiverImage ? receiverImage->GetDheCurrentType(&klass->byval_arg) : nullptr;
		if (currentType)
			currentClass = il2cpp::vm::Class::FromIl2CppType(currentType);
		else if (!IsInterpreterType(klass))
		{
			// An external AOT implementation still owns its original interface slots.
			if (logicalSlot < interfaceType->method_count)
				return false;
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetMissingMethodException(
				"The AOT receiver has no current DHE interface implementation."));
		}
		il2cpp::vm::Class::Init(currentClass);
#if UNITY_ENGINE_TUANJIE
		il2cpp::vm::Class::SetupVTable(currentClass);
#endif
		for (uint16_t index = 0; index < currentClass->interface_offsets_count; ++index)
		{
			const Il2CppRuntimeInterfaceOffsetPair& pair = currentClass->interfaceOffsets[index];
			if (pair.interfaceType != interfaceType)
				continue;
			const int64_t currentSlot = static_cast<int64_t>(pair.offset) + currentInterfaceMethod->slot;
			if (pair.offset < 0 || currentSlot < 0 || currentSlot >= currentClass->vtable_count)
				RaiseExecutionEngineException("DHE current interface slot exceeds its receiver vtable.");
			const MethodInfo* target = currentClass->vtable[currentSlot].method;
			if (!target || !target->klass || IsAbstractMethod(target->flags))
				il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetMissingMethodException(
					"The current DHE interface method has no implementation."));
			AOTHomologousImage* targetImage = AOTHomologousImage::FindImageByAssembly(target->klass->image->assembly);
			if (targetImage && dhe::IsDheAssembly(target->klass->image->assembly))
				target = targetImage->ResolveLogicalMethod(target);
#if HYBRIDCLR_UNITY_2021
			Il2CppMethodPointer pointer = target->is_generic ? nullptr : il2cpp::vm::Method::GetVirtualCallMethodPointer(target);
#else
			Il2CppMethodPointer pointer = target->is_generic ? nullptr : ReadPublishedPointer(&const_cast<MethodInfo*>(target)->virtualMethodPointer);
#endif
			// The generic interface caller needs the definition before it can
			// inflate a callable implementation. A null entry is valid only
			// for that open method definition, as in an ordinary IL2CPP vtable.
			if (!pointer && !target->is_generic)
				pointer = InitAndGetInterpreterDirectlyCallVirtualMethodPointer(target);
			if (!pointer && !target->is_generic)
				RaiseExecutionEngineException("DHE interface implementation has no callable entry.");
			VirtualInvokeData entry = {};
			entry.method = target;
			entry.methodPtr = pointer;
			result = &s_dheInterfaceDispatch[klass][interfaceType].emplace(logicalSlot, entry).first->second;
			return true;
		}
		il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetMissingMethodException(
			"The current DHE receiver does not implement the requested interface."));
		return false;
	}

	const PropertyInfo* MetadataModule::GetDheCustomAttributeProperty(Il2CppClass* klass, uint32_t index)
	{
		return GetDheAttributePropertyByIndex(GetDheSupplementalImage(klass->image), klass, index);
	}

	const MethodInfo* MetadataModule::ResolveDheMethod(const MethodInfo* method)
	{
		if (!method || !method->klass || !method->klass->image ||
			!IsInterpreterImage(method->klass->image) ||
			!dhe::IsDheAssembly(method->klass->image->assembly))
			return method;
		AOTHomologousImage* image = AOTHomologousImage::FindImageByAssembly(method->klass->image->assembly);
		return image ? image->ResolveLogicalMethod(method) : method;
	}

	bool MetadataModule::TryGetDheCustomAttributeSource(const Il2CppImage* image,
		uint32_t token, const Il2CppImage*& sourceImage, uint32_t& sourceToken)
	{
		AOTHomologousImage* homologous = GetDheSupplementalImage(image);
		return homologous && homologous->TryGetCustomAttributeSource(
			token, sourceImage, sourceToken);
	}

    const Il2CppImage* MetadataModule::GetDheMethodMetadataImage(const MethodInfo* method)
    {
        const Il2CppImage* image = method->klass->image;
        AOTHomologousImage* homologous = GetDheSupplementalImage(image);
        Image* supplemental = homologous ? homologous->GetSupplementalMethodImage(method) : nullptr;
        return supplemental ? static_cast<InterpreterImage*>(supplemental)->GetIl2CppImage() : image;
    }

    bool MetadataModule::TryGetDheReferencedAssemblies(const Il2CppAssembly* assembly,
        std::vector<const Il2CppAssemblyName*>& references)
    {
        AOTHomologousImage* homologous = GetDheSupplementalImage(assembly->image);
        if (!homologous)
        {
            return false;
        }
        RawImageBase& raw = homologous->GetRawImage();
        std::vector<const Il2CppAssemblyName*> currentReferences;
        const uint32_t count = raw.GetTable(TableType::ASSEMBLYREF).rowNum;
        currentReferences.reserve(count);
        for (uint32_t row = 1; row <= count; ++row)
        {
            TbAssemblyRef reference = raw.ReadAssemblyRef(row);
            const char* name = raw.GetStringFromRawIndex(reference.name);
            const Il2CppAssembly* referencedAssembly = il2cpp::vm::Assembly::GetLoadedAssembly(name);
            if (!referencedAssembly)
            {
                il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetDllNotFoundException(name));
            }
            currentReferences.push_back(&referencedAssembly->aname);
        }
        references.insert(references.end(), currentReferences.begin(), currentReferences.end());
        return true;
    }

    Il2CppClass* MetadataModule::FindDheSupplementalType(const Il2CppImage* image,
        const char* namespaze, const char* name)
    {
        AOTHomologousImage* homologous = GetDheSupplementalImage(image);
        return homologous ? homologous->FindSupplementalType(namespaze, name) : nullptr;
    }

    void MetadataModule::GetDheSupplementalTypes(const Il2CppImage* image,
        std::vector<const Il2CppClass*>& types)
    {
        AOTHomologousImage* homologous = GetDheSupplementalImage(image);
        if (homologous)
        {
            homologous->GetSupplementalTypes(types);
        }
    }

    Il2CppClass* MetadataModule::GetFirstDheSupplementalNestedType(Il2CppClass* klass,
        void** iter)
    {
        AOTHomologousImage* homologous = klass && klass->image
            ? GetDheSupplementalImage(klass->image) : nullptr;
        return homologous ? homologous->GetFirstSupplementalNestedType(klass, iter) : nullptr;
    }

    bool MetadataModule::TryGetNextDheSupplementalNestedType(Il2CppClass* klass, void** iter,
        Il2CppClass** nestedType)
    {
        AOTHomologousImage* homologous = klass && klass->image
            ? GetDheSupplementalImage(klass->image) : nullptr;
        return homologous && homologous->TryGetNextSupplementalNestedType(
            klass, iter, nestedType);
    }

    const MethodInfo* MetadataModule::GetFirstDheSupplementalMethod(Il2CppClass* klass,
        void** iter)
    {
        AOTHomologousImage* homologous = klass && klass->image
            ? GetDheSupplementalImage(klass->image) : nullptr;
        return homologous ? homologous->GetFirstSupplementalMethod(klass, iter) : nullptr;
    }

	bool MetadataModule::TryGetNextDheSupplementalMethod(Il2CppClass* klass, void** iter,
		const MethodInfo** method)
    {
        AOTHomologousImage* homologous = klass && klass->image
            ? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous && homologous->TryGetNextSupplementalMethod(klass, iter, method);
	}

	size_t MetadataModule::GetDheRemovedMethodCount(Il2CppClass* klass)
	{
		if (!klass || !klass->methods)
		{
			return 0;
		}
		size_t count = 0;
		for (uint16_t index = 0; index < klass->method_count; ++index)
		{
			if (dhe::IsRemovedMethod(klass->methods[index]))
			{
				++count;
			}
		}
		return count;
	}

	size_t MetadataModule::GetDheSupplementalMethodCount(Il2CppClass* klass)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous ? homologous->GetSupplementalMethodCount(klass) : 0;
	}

	bool MetadataModule::IsDheRemovedField(const FieldInfo* field)
	{
		if (!field || !field->parent || !field->parent->image ||
			!dhe::IsDheAssembly(field->parent->image->assembly))
		{
			return false;
		}
		AOTHomologousImage* homologous = GetDheSupplementalImage(field->parent->image);
		return homologous && homologous->IsRemovedField(field);
	}

	size_t MetadataModule::GetDheRemovedFieldCount(Il2CppClass* klass)
	{
		if (!klass || !klass->fields)
		{
			return 0;
		}
		size_t count = 0;
		for (uint16_t index = 0; index < klass->field_count; ++index)
		{
			if (IsDheRemovedField(klass->fields + index))
			{
				++count;
			}
		}
		return count;
	}

	FieldInfo* MetadataModule::GetFirstDheSupplementalField(Il2CppClass* klass,
		void** iter)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous ? homologous->GetFirstSupplementalField(klass, iter) : nullptr;
	}

	bool MetadataModule::TryGetNextDheSupplementalField(Il2CppClass* klass, void** iter,
		FieldInfo** field)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous && homologous->TryGetNextSupplementalField(klass, iter, field);
	}

	size_t MetadataModule::GetDheSupplementalFieldCount(Il2CppClass* klass)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous ? homologous->GetSupplementalFieldCount(klass) : 0;
	}

	Il2CppClass* MetadataModule::GetDheLogicalFieldParent(FieldInfo* field)
	{
		if (!field || !field->parent || !field->parent->image ||
			!field->parent->image->assembly)
		{
			return field ? field->parent : nullptr;
		}
		AOTHomologousImage* homologous = AOTHomologousImage::FindImageByAssembly(
			field->parent->image->assembly);
		Il2CppClass* logical = homologous
			? homologous->GetSupplementalFieldLogicalParent(field) : nullptr;
		return logical ? logical : field->parent;
	}

	const FieldInfo* MetadataModule::ResolveDheSupplementalField(const FieldInfo* field)
	{
		if (!field || !field->parent || !field->parent->image ||
			!dhe::IsDheAssembly(field->parent->image->assembly))
			return field;
		AOTHomologousImage* image = AOTHomologousImage::FindImageByAssembly(field->parent->image->assembly);
		return image ? image->ResolveSupplementalField(field) : field;
	}

	const Il2CppFieldDefinition* MetadataModule::ResolveDheSupplementalFieldDefinition(
		const Il2CppType* type, const char* name, const Il2CppType* fieldType)
	{
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
		AOTHomologousImage* image = GetDheSupplementalImage(klass->image);
		return image ? image->ResolveSupplementalFieldDefinition(type, name, fieldType) : nullptr;
	}

	void MetadataModule::RegisterDheSupplementalInstanceField(FieldInfo* runtimeField,
		FieldInfo* logicalField, const FieldInfo* definitionField)
	{
		std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
		if (definitionField)
		{
			auto definition = s_dheInstanceFieldSlots.find(definitionField);
			if (definition == s_dheInstanceFieldSlots.end())
				RaiseExecutionEngineException("DHE generic field definition has no sidecar slot.");
			DheInstanceFieldSlot slot = { definition->second.slot, logicalField };
			s_dheInstanceFieldSlots[runtimeField] = slot;
			s_dheInstanceFieldSlots[logicalField] = slot;
			return;
		}
		auto runtimeSlot = s_dheInstanceFieldSlots.find(runtimeField);
		if (runtimeSlot != s_dheInstanceFieldSlots.end())
		{
			s_dheInstanceFieldSlots[logicalField] = runtimeSlot->second;
			return;
		}
		DheInstanceFieldSlot slot = { s_dheInstanceFieldSlotCount++, logicalField };
		s_dheInstanceFieldSlots[runtimeField] = slot;
		s_dheInstanceFieldSlots[logicalField] = slot;
	}

	bool MetadataModule::IsDheSupplementalInstanceField(const FieldInfo* field)
	{
		std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
		return s_dheInstanceFieldSlots.find(field) != s_dheInstanceFieldSlots.end();
	}

	bool MetadataModule::TryGetDheSupplementalInstanceFieldValue(Il2CppObject* obj,
		FieldInfo* field, void* value)
	{
		Il2CppObject* cell;
		FieldInfo* valueField;
		if (!TryGetDheFieldCell(obj, field, cell, valueField))
			return false;
		il2cpp::vm::Field::GetValue(cell, valueField, value);
		return true;
	}

	bool MetadataModule::TrySetDheSupplementalInstanceFieldValue(Il2CppObject* obj,
		const FieldInfo* field, void* value, bool dereferencePointer)
	{
		Il2CppObject* cell;
		FieldInfo* valueField;
		if (!TryGetDheFieldCell(obj, field, cell, valueField))
			return false;
		il2cpp::vm::Field::SetValueRaw(valueField->type,
			reinterpret_cast<uint8_t*>(cell) + valueField->offset, value, dereferencePointer);
		return true;
	}

	bool MetadataModule::TryGetDheSupplementalInstanceFieldAddress(Il2CppObject* obj,
		const FieldInfo* field, void** address)
	{
		Il2CppObject* cell;
		FieldInfo* valueField;
		if (!TryGetDheFieldCell(obj, field, cell, valueField))
			return false;
		*address = reinterpret_cast<uint8_t*>(cell) + valueField->offset;
		return true;
	}

	bool MetadataModule::TryGetDheSupplementalInstanceFieldValueObject(Il2CppObject* obj,
		FieldInfo* field, Il2CppObject** value)
	{
		Il2CppObject* cell;
		FieldInfo* valueField;
		if (!TryGetDheFieldCell(obj, field, cell, valueField))
			return false;
		*value = il2cpp::vm::Field::GetValueObject(valueField, cell);
		return true;
	}

	bool MetadataModule::TrySetDheSupplementalInstanceFieldValueObject(Il2CppObject* obj,
		FieldInfo* field, Il2CppObject* value)
	{
		Il2CppObject* cell;
		FieldInfo* valueField;
		if (!TryGetDheFieldCell(obj, field, cell, valueField))
			return false;
		Il2CppClass* fieldType = il2cpp::vm::Class::FromIl2CppType(valueField->type);
		if (il2cpp::vm::Class::IsNullable(fieldType))
			il2cpp::vm::Object::UnboxNullableWithWriteBarrier(value, fieldType,
				reinterpret_cast<uint8_t*>(cell) + valueField->offset);
		else
			il2cpp::vm::Field::SetValue(cell, valueField,
				value && fieldType->byval_arg.valuetype ? il2cpp::vm::Object::Unbox(value) : value);
		return true;
	}

	bool MetadataModule::HasDheLogicalPropertyView(Il2CppClass* klass)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous && homologous->HasLogicalPropertyView(klass);
	}

	const PropertyInfo* MetadataModule::GetFirstDheLogicalProperty(Il2CppClass* klass,
		void** iter)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous ? homologous->GetFirstLogicalProperty(klass, iter) : nullptr;
	}

	bool MetadataModule::TryGetNextDheLogicalProperty(Il2CppClass* klass, void** iter,
		const PropertyInfo** property)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous && homologous->TryGetNextLogicalProperty(klass, iter, property);
	}

	size_t MetadataModule::GetDheLogicalPropertyCount(Il2CppClass* klass)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous ? homologous->GetLogicalPropertyCount(klass) : 0;
	}

	bool MetadataModule::HasDheLogicalEventView(Il2CppClass* klass)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous && homologous->HasLogicalEventView(klass);
	}

	const EventInfo* MetadataModule::GetFirstDheLogicalEvent(Il2CppClass* klass,
		void** iter)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous ? homologous->GetFirstLogicalEvent(klass, iter) : nullptr;
	}

	bool MetadataModule::TryGetNextDheLogicalEvent(Il2CppClass* klass, void** iter,
		const EventInfo** eventInfo)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous && homologous->TryGetNextLogicalEvent(klass, iter, eventInfo);
	}

	size_t MetadataModule::GetDheLogicalEventCount(Il2CppClass* klass)
	{
		AOTHomologousImage* homologous = klass && klass->image
			? GetDheSupplementalImage(klass->image) : nullptr;
		return homologous ? homologous->GetLogicalEventCount(klass) : 0;
	}
}
}
