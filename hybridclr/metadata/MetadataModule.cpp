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

	const PropertyInfo* MetadataModule::GetDheCustomAttributeProperty(Il2CppClass* klass, uint32_t index)
	{
		return GetDheAttributePropertyByIndex(GetDheSupplementalImage(klass->image), klass, index);
	}

	const MethodInfo* MetadataModule::ResolveDheCustomAttributeConstructor(const MethodInfo* method)
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
		uint32_t slot;
		FieldInfo* logicalField;
		Il2CppObject* stored;
		{
			std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
			if (!TryGetDheFieldSlotLocked(field, slot, logicalField))
				return false;
			stored = GetDheSidecarValueLocked(obj, slot);
		}
		if (!stored)
		{
			il2cpp::vm::Field::SetValueRaw(logicalField->type, value, nullptr, false);
			return true;
		}
		Il2CppClass* fieldType = il2cpp::vm::Class::FromIl2CppType(logicalField->type);
		if (il2cpp::vm::Class::IsNullable(fieldType))
		{
			il2cpp::vm::Object::UnboxNullable(stored, fieldType, value);
		}
		else
		{
			il2cpp::vm::Field::SetValueRaw(logicalField->type, value,
				fieldType->byval_arg.valuetype ? il2cpp::vm::Object::Unbox(stored) : stored, false);
		}
		return true;
	}

	bool MetadataModule::TrySetDheSupplementalInstanceFieldValue(Il2CppObject* obj,
		const FieldInfo* field, void* value)
	{
		uint32_t slot;
		FieldInfo* logicalField;
		{
			std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
			if (!TryGetDheFieldSlotLocked(field, slot, logicalField))
			{
				return false;
			}
		}
		Il2CppClass* fieldType = il2cpp::vm::Class::FromIl2CppType(logicalField->type);
		Il2CppObject* boxed = il2cpp::vm::Object::Box(fieldType, value);
		// Sidecar allocation can enter engine metadata; keep one lock order.
		il2cpp::os::FastAutoLock metadataLock(&il2cpp::vm::g_MetadataLock);
		std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
		SetDheSidecarValueLocked(obj, slot, boxed);
		return true;
	}

	bool MetadataModule::TryGetDheSupplementalInstanceFieldValueObject(Il2CppObject* obj,
		FieldInfo* field, Il2CppObject** value)
	{
		uint32_t slot;
		FieldInfo* logicalField;
		Il2CppObject* stored;
		{
			std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
			if (!TryGetDheFieldSlotLocked(field, slot, logicalField))
				return false;
			stored = GetDheSidecarValueLocked(obj, slot);
		}
		Il2CppClass* fieldType = il2cpp::vm::Class::FromIl2CppType(logicalField->type);
		if (!stored && fieldType->byval_arg.valuetype && !il2cpp::vm::Class::IsNullable(fieldType))
		{
			stored = il2cpp::vm::Object::New(fieldType);
		}
		else if (stored && fieldType->byval_arg.valuetype)
		{
			stored = il2cpp::vm::Object::Box(stored->klass, il2cpp::vm::Object::Unbox(stored));
		}
		*value = stored;
		return true;
	}

	bool MetadataModule::TrySetDheSupplementalInstanceFieldValueObject(Il2CppObject* obj,
		FieldInfo* field, Il2CppObject* value)
	{
		uint32_t slot;
		FieldInfo* logicalField;
		{
			std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
			if (!TryGetDheFieldSlotLocked(field, slot, logicalField))
				return false;
		}
		Il2CppClass* fieldType = il2cpp::vm::Class::FromIl2CppType(logicalField->type);
		if (value && fieldType->byval_arg.valuetype)
			value = il2cpp::vm::Object::Box(value->klass, il2cpp::vm::Object::Unbox(value));
		il2cpp::os::FastAutoLock metadataLock(&il2cpp::vm::g_MetadataLock);
		std::lock_guard<std::mutex> lock(s_dheSidecarMutex);
		SetDheSidecarValueLocked(obj, slot, value);
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
