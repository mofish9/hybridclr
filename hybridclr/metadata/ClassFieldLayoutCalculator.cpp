#if defined(__has_include)
#if __has_include("../lab/InstrumentationConfig.h")
#include "../lab/InstrumentationConfig.h"
#endif
#endif

#include "ClassFieldLayoutCalculator.h"

#include "metadata/FieldLayout.h"
#include "metadata/GenericMetadata.h"
#include "vm/Field.h"
#include "vm/GlobalMetadata.h"

#include "InterpreterImage.h"
#include "MetadataModule.h"

namespace hybridclr
{
namespace metadata
{
    typedef void* voidptr_t;
#define IL2CPP_ALIGN_STRUCT(type) struct type ## AlignStruct {uint8_t pad; type t; };

    IL2CPP_ALIGN_STRUCT(voidptr_t)
        IL2CPP_ALIGN_STRUCT(int8_t)
        IL2CPP_ALIGN_STRUCT(int16_t)
        IL2CPP_ALIGN_STRUCT(int32_t)
        IL2CPP_ALIGN_STRUCT(int64_t)
        IL2CPP_ALIGN_STRUCT(intptr_t)
        IL2CPP_ALIGN_STRUCT(float)
        IL2CPP_ALIGN_STRUCT(double)

#define IL2CPP_ALIGN_OF(type) ((int32_t)offsetof(type ## AlignStruct, t))

    ClassFieldLayoutCalculator::ClassFieldLayoutCalculator(InterpreterImage* image)
        : _image(image)
    {
        // The calculator is used once per interpreter image. Reserve the small
        // scalar caches up front so a wide type graph does not repeatedly
        // rehash while first-touch layout walks nested value types.
        const size_t typeCount = image != nullptr ? image->GetTypeDefinitionCount() : 0;
        _classLayoutByTypeIndex.assign(typeCount, nullptr);
        _sizeAndAlignmentCache.reserve(typeCount);
        _blittableCache.reserve(typeCount);
    }

    ClassFieldLayoutCalculator::~ClassFieldLayoutCalculator()
    {
        FlushInstrumentation();
        for (auto it : _classMap)
        {
            ClassLayoutInfo* info = it.second;
            info->~ClassLayoutInfo();
            HYBRIDCLR_FREE(info);
        }
    }

    void ClassFieldLayoutCalculator::FlushInstrumentation()
    {
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
        if (_instrumentationFlushed)
        {
            return;
        }
        RecordMetadataInitStage(_image->GetIndex(), "ClassLayoutSizeCacheHit", _sizeAndAlignmentCacheHits);
        RecordMetadataInitStage(_image->GetIndex(), "ClassLayoutSizeCacheMiss", _sizeAndAlignmentCacheMisses);
        RecordMetadataInitStage(_image->GetIndex(), "ClassLayoutBlittableCacheHit", _blittableCacheHits);
        RecordMetadataInitStage(_image->GetIndex(), "ClassLayoutBlittableCacheMiss", _blittableCacheMisses);
        _instrumentationFlushed = true;
#endif
    }

    bool ClassFieldLayoutCalculator::ShouldCacheType(const Il2CppType* type) const
    {
        if (type == nullptr || type->byref)
        {
            return false;
        }
        if (type->type == IL2CPP_TYPE_VALUETYPE)
        {
            return true;
        }
        return type->type == IL2CPP_TYPE_GENERICINST && IsValueType(GetUnderlyingTypeDefinition(type));
    }

    bool ClassFieldLayoutCalculator::TryGetLocalTypeIndex(const Il2CppType* type, uint32_t& index) const
    {
        if (type == nullptr || (type->type != IL2CPP_TYPE_CLASS && type->type != IL2CPP_TYPE_VALUETYPE))
        {
            return false;
        }

        const Il2CppTypeDefinition* typeDef = GetUnderlyingTypeDefinition(type);
        if (typeDef == nullptr || !IsInterpreterType(typeDef)
            || typeDef->genericContainerIndex != kGenericContainerIndexInvalid
            || DecodeImageIndex(typeDef->byvalTypeIndex) != _image->GetIndex())
        {
            return false;
        }

        index = _image->GetTypeRawIndex(typeDef);
        return index < _classLayoutByTypeIndex.size();
    }

    ClassLayoutInfo* ClassFieldLayoutCalculator::GetClassLayoutInfo(const Il2CppType* type)
    {
        uint32_t index = 0;
        if (TryGetLocalTypeIndex(type, index))
        {
            ClassLayoutInfo* layout = _classLayoutByTypeIndex[index];
            if (layout != nullptr)
            {
                return layout;
            }
        }

        auto it = _classMap.find(type);
        return it != _classMap.end() ? it->second : nullptr;
    }

    SizeAndAlignment ClassFieldLayoutCalculator::GetTypeSizeAndAlignment(const Il2CppType* type)
    {
        if (!ShouldCacheType(type))
        {
            return GetTypeSizeAndAlignmentUncached(type);
        }
        auto it = _sizeAndAlignmentCache.find(type);
        if (it != _sizeAndAlignmentCache.end())
        {
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
            ++_sizeAndAlignmentCacheHits;
#endif
            return it->second;
        }
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
        ++_sizeAndAlignmentCacheMisses;
#endif
        SizeAndAlignment result = GetTypeSizeAndAlignmentUncached(type);
        _sizeAndAlignmentCache.emplace(type, result);
        return result;
    }

    SizeAndAlignment ClassFieldLayoutCalculator::GetTypeSizeAndAlignmentUncached(const Il2CppType* type)
    {
        SizeAndAlignment sa = { };
        if (type->byref)
        {
            sa.size = sa.nativeSize = sizeof(voidptr_t);
            sa.alignment = IL2CPP_ALIGN_OF(voidptr_t);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        }

        switch (type->type)
        {
        case IL2CPP_TYPE_I1:
        case IL2CPP_TYPE_U1:
        case IL2CPP_TYPE_BOOLEAN:
            sa.size = sa.nativeSize = sizeof(int8_t);
            sa.alignment = IL2CPP_ALIGN_OF(int8_t);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        case IL2CPP_TYPE_I2:
        case IL2CPP_TYPE_U2:
        case IL2CPP_TYPE_CHAR:
            sa.size = sa.nativeSize = sizeof(int16_t);
            sa.alignment = IL2CPP_ALIGN_OF(int16_t);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        case IL2CPP_TYPE_I4:
        case IL2CPP_TYPE_U4:
            sa.size = sa.nativeSize = sizeof(int32_t);
            sa.alignment = IL2CPP_ALIGN_OF(int32_t);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        case IL2CPP_TYPE_I8:
        case IL2CPP_TYPE_U8:
            sa.size = sa.nativeSize = sizeof(int64_t);
            sa.alignment = IL2CPP_ALIGN_OF(int64_t);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        case IL2CPP_TYPE_I:
        case IL2CPP_TYPE_U:
            // TODO should we use pointer or int32_t here?
            sa.size = sa.nativeSize = sizeof(intptr_t);
            sa.alignment = IL2CPP_ALIGN_OF(intptr_t);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        case IL2CPP_TYPE_R4:
            sa.size = sa.nativeSize = sizeof(float);
            sa.alignment = IL2CPP_ALIGN_OF(float);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        case IL2CPP_TYPE_R8:
            sa.size = sa.nativeSize = sizeof(double);
            sa.alignment = IL2CPP_ALIGN_OF(double);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        case IL2CPP_TYPE_PTR:
        case IL2CPP_TYPE_FNPTR:
        case IL2CPP_TYPE_STRING:
        case IL2CPP_TYPE_SZARRAY:
        case IL2CPP_TYPE_ARRAY:
        case IL2CPP_TYPE_CLASS:
        case IL2CPP_TYPE_OBJECT:
            sa.size = sa.nativeSize = sizeof(voidptr_t);
            sa.alignment = IL2CPP_ALIGN_OF(voidptr_t);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        case IL2CPP_TYPE_VAR:
        case IL2CPP_TYPE_MVAR:
            sa.size = sa.nativeSize = 1;
            sa.alignment = 1;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = sa.alignment;
#endif
            return sa;
        case IL2CPP_TYPE_VALUETYPE:
        {
            CalcClassNotStaticFields(type);
            ClassLayoutInfo* classLayout = GetClassLayoutInfo(type);
            IL2CPP_ASSERT(classLayout);
            sa.size = classLayout->instanceSize - sizeof(Il2CppObject);
            sa.nativeSize = classLayout->nativeSize;
            sa.alignment = classLayout->alignment;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            sa.naturalAlignment = classLayout->naturalAlignment;
#endif
            return sa;
        }
        case IL2CPP_TYPE_GENERICINST:
        {
            Il2CppGenericClass* gclass = type->data.generic_class;
            //Il2CppClass* container_class = GenericClass::GetTypeDefinition(gclass);
            const Il2CppTypeDefinition* typeDef = GetUnderlyingTypeDefinition(type);
            if (IsValueType(typeDef))
            {
                CalcClassNotStaticFields(type);
                ClassLayoutInfo* classLayout = GetClassLayoutInfo(type);
                IL2CPP_ASSERT(classLayout);
                sa.size = classLayout->instanceSize - sizeof(Il2CppObject);
                sa.nativeSize = classLayout->nativeSize;
                sa.alignment = classLayout->alignment;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
                sa.naturalAlignment = classLayout->naturalAlignment;
#endif
            }
            else
            {
                sa.size = sa.nativeSize = sizeof(voidptr_t);
                sa.alignment = IL2CPP_ALIGN_OF(voidptr_t);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
                sa.naturalAlignment = sa.alignment;
#endif
            }
            return sa;
        }
        default:
            IL2CPP_ASSERT(0);
            break;
        }
        return sa;
    }

    static int32_t AlignTo(int32_t size, int32_t alignment)
    {
        if (size & (alignment - 1))
        {
            size += alignment - 1;
            size &= ~(alignment - 1);
        }

        return size;
    }

    void ClassFieldLayoutCalculator::LayoutFields(int32_t actualParentSize, int32_t parentAlignment, uint8_t packing, std::vector<FieldLayout>& fields, FieldLayoutKind kind, FieldLayoutData& data)
    {
        //data.classSize = parentSize;
        data.actualClassSize = actualParentSize;
        IL2CPP_ASSERT(parentAlignment <= std::numeric_limits<uint8_t>::max());
        data.minimumAlignment = static_cast<uint8_t>(parentAlignment);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
        data.naturalAlignment = 0;
#endif
        data.nativeSize = 0;
        for (FieldLayout& field : fields)
        {
			bool selected = kind == FieldLayoutKind::Instance ? IsInstanceField(field.type)
				: (kind == FieldLayoutKind::Static ? field.isNormalStatic : field.isThreadStatic);
			if (!selected)
			{
				continue;
			}
			SizeAndAlignment sa = GetTypeSizeAndAlignment(field.type);
			field.size = sa.size;// sa.nativeSize > 0 ? sa.nativeSize : sa.size;

            // For fields, we might not want to take the actual alignment of the type - that might account for
            // packing. When a type is used as a field, we should not care about its alignment with packing,
            // instead let's use its natural alignment, without regard for packing. So if it's alignment
            // is less than the compiler's minimum alignment (4 bytes), lets use the natural alignment if we have it.
            uint8_t alignment = sa.alignment;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            if (alignment < 4 && sa.naturalAlignment != 0)
                alignment = sa.naturalAlignment;
#endif
            if (packing != 0)
                alignment = std::min(sa.alignment, packing);
            int32_t offset = data.actualClassSize;

            offset += alignment - 1;
            offset &= ~(alignment - 1);
			field.offset = offset;

            data.actualClassSize = offset + std::max(sa.size, (int32_t)1);
            data.minimumAlignment = std::max(data.minimumAlignment, alignment);
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            data.naturalAlignment = std::max({ data.naturalAlignment, sa.alignment, sa.naturalAlignment });
#endif
            data.nativeSize += sa.size;
        }

        data.classSize = AlignTo(data.actualClassSize, data.minimumAlignment);

        // C++ ABI difference between MS and Clang
#if IL2CPP_CXX_ABI_MSVC
        data.actualClassSize = data.classSize;
#endif
    }

    bool ClassFieldLayoutCalculator::IsBlittable(const Il2CppType* type)
    {
        if (!ShouldCacheType(type))
        {
            return IsBlittableUncached(type);
        }
        auto it = _blittableCache.find(type);
        if (it != _blittableCache.end())
        {
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
            ++_blittableCacheHits;
#endif
            return it->second;
        }
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
        ++_blittableCacheMisses;
#endif
        bool result = IsBlittableUncached(type);
        _blittableCache.emplace(type, result);
        return result;
    }

    bool ClassFieldLayoutCalculator::IsBlittableUncached(const Il2CppType* type)
    {
        if (type->byref)
        {
            return true;
        }

        switch (type->type)
        {
        case IL2CPP_TYPE_I1:
        case IL2CPP_TYPE_U1:
        case IL2CPP_TYPE_BOOLEAN:
        case IL2CPP_TYPE_I2:
        case IL2CPP_TYPE_U2:
        case IL2CPP_TYPE_CHAR:
        case IL2CPP_TYPE_I4:
        case IL2CPP_TYPE_U4:
        case IL2CPP_TYPE_I8:
        case IL2CPP_TYPE_U8:
        case IL2CPP_TYPE_I:
        case IL2CPP_TYPE_U:
        case IL2CPP_TYPE_R4:
        case IL2CPP_TYPE_R8:
        case IL2CPP_TYPE_PTR:
        case IL2CPP_TYPE_FNPTR:
            return true;
        case IL2CPP_TYPE_STRING:
        case IL2CPP_TYPE_SZARRAY:
        case IL2CPP_TYPE_ARRAY:
        case IL2CPP_TYPE_CLASS:
        case IL2CPP_TYPE_OBJECT:
        case IL2CPP_TYPE_VAR:
        case IL2CPP_TYPE_MVAR:
            return false;
        case IL2CPP_TYPE_VALUETYPE:
        {
            CalcClassNotStaticFields(type);
            ClassLayoutInfo* classLayout = GetClassLayoutInfo(type);
            IL2CPP_ASSERT(classLayout);
            return classLayout->blittable;
        }
        case IL2CPP_TYPE_GENERICINST:
        {
            const Il2CppTypeDefinition* typeDef = GetUnderlyingTypeDefinition(type);
            if (IsValueType(typeDef))
            {
                CalcClassNotStaticFields(type);
                ClassLayoutInfo* classLayout = GetClassLayoutInfo(type);
                IL2CPP_ASSERT(classLayout);
                return classLayout->blittable;
            }
            else
            {
                return false;
            }
        }
        default:
            IL2CPP_ASSERT(0);
            return false;
        }
    }


    inline bool IsRawNormalStaticField(const Il2CppType* type, int32_t offset)
    {
        if ((type->attrs & FIELD_ATTRIBUTE_STATIC) == 0)
            return false;

        if (offset == THREAD_LOCAL_STATIC_MASK)
            return false;

        if ((type->attrs & FIELD_ATTRIBUTE_LITERAL) != 0)
            return false;

        return true;
    }

    inline bool IsRawThreadStaticField(const Il2CppType* type, int32_t offset)
    {
        if ((type->attrs & FIELD_ATTRIBUTE_STATIC) == 0)
            return false;

        if (offset != THREAD_LOCAL_STATIC_MASK)
            return false;

        if ((type->attrs & FIELD_ATTRIBUTE_LITERAL) != 0)
            return false;

        return true;
    }

	void ClassFieldLayoutCalculator::CalcClassNotStaticFields(const Il2CppType* type)
	{
		auto insertion = _classMap.insert({ type, nullptr });
		if (!insertion.second)
		{
			return;
		}
		ClassLayoutInfo* layoutStorage = new (HYBRIDCLR_MALLOC_ZERO(sizeof(ClassLayoutInfo))) ClassLayoutInfo();
		insertion.first->second = layoutStorage;
		uint32_t localTypeIndex = 0;
		if (TryGetLocalTypeIndex(type, localTypeIndex))
		{
			_classLayoutByTypeIndex[localTypeIndex] = layoutStorage;
		}
		ClassLayoutInfo& layout = *layoutStorage;
		layout.type = type;
        const Il2CppTypeDefinition* typeDef = GetUnderlyingTypeDefinition(type);
        std::vector<FieldLayout>& fields = layout.fields;
        fields.resize(typeDef->field_count, {});

        bool isCurAssemblyType = DecodeImageIndex(typeDef->byvalTypeIndex) == _image->GetIndex();
        if (isCurAssemblyType)
        {
            // SetupFieldsLocked and the transform path already hold g_MetadataLock here.
            _image->EnsureTypeFieldMetadataInitializedLocked(typeDef);
        }
        if ((type->type == IL2CPP_TYPE_VALUETYPE || type->type == IL2CPP_TYPE_CLASS) && !isCurAssemblyType)
        {
            Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
            il2cpp::vm::Class::SetupFields(klass);
            layout.instanceSize = klass->instance_size;
            layout.actualSize = klass->actualSize;
            layout.nativeSize = klass->native_size;
            layout.alignment = klass->minimumAlignment;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            layout.naturalAlignment = klass->naturalAligment;
#endif
            layout.blittable = il2cpp::vm::Class::IsBlittable(klass);
            return;
        }

        // Most metadata types have no fields of their own. Their layout is either
        // the fixed empty value type layout or exactly the inherited reference type
        // layout, so avoid allocating/iterating the normal field-layout path.
        if (typeDef->field_count == 0)
        {
            if (il2cpp::metadata::GenericMetadata::ContainsGenericParameters(type)
                || ((type->type == IL2CPP_TYPE_VALUETYPE || type->type == IL2CPP_TYPE_CLASS)
                    && typeDef->genericContainerIndex != kGenericContainerIndexInvalid))
            {
                layout.instanceSize = 0;
                layout.actualSize = 0;
                layout.nativeSize = -1;
                layout.alignment = 1;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
                layout.naturalAlignment = 1;
#endif
                layout.blittable = false;
                return;
            }

            // C# static classes are emitted as abstract + sealed reference types.
            // They cannot have instance fields or a user-defined instance base, so
            // resolving their layout does not need to recurse through System.Object.
            // Keep an explicit metadata class size as an upper bound for malformed
            // or hand-authored metadata rather than silently discarding it.
            if (type->type == IL2CPP_TYPE_CLASS
                && (typeDef->flags & TYPE_ATTRIBUTE_ABSTRACT) != 0
                && (typeDef->flags & TYPE_ATTRIBUTE_SEALED) != 0)
            {
                int32_t staticClassSize = 0;
                if (IsInterpreterType(typeDef))
                {
                    staticClassSize = (int32_t)MetadataModule::GetImage(typeDef)->GetClassLayout(typeDef).classSize;
                }
                layout.instanceSize = std::max((int32_t)sizeof(Il2CppObject), staticClassSize + (int32_t)sizeof(Il2CppObject));
                layout.actualSize = layout.instanceSize;
                layout.nativeSize = -1;
                layout.alignment = PTR_SIZE;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
                layout.naturalAlignment = PTR_SIZE;
#endif
                layout.blittable = false;
                return;
            }

            int32_t layoutClassSize = 0;
            if (IsInterpreterType(typeDef))
            {
                layoutClassSize = (int32_t)MetadataModule::GetImage(typeDef)->GetClassLayout(typeDef).classSize;
            }

            if (IsValueType(typeDef))
            {
                layout.instanceSize = IL2CPP_SIZEOF_STRUCT_WITH_NO_INSTANCE_FIELDS + sizeof(Il2CppObject);
                layout.actualSize = layout.instanceSize;
                layout.nativeSize = IL2CPP_SIZEOF_STRUCT_WITH_NO_INSTANCE_FIELDS;
                layout.alignment = 1;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
                layout.naturalAlignment = 1;
#endif
                layout.blittable = true;
                if (layoutClassSize > 0)
                {
                    layout.instanceSize = std::max(layout.instanceSize, layoutClassSize + (int32_t)sizeof(Il2CppObject));
                    layout.actualSize = layout.instanceSize;
                    layout.nativeSize = std::max(layout.nativeSize, layoutClassSize);
                }
                return;
            }

            const Il2CppType* parentType = nullptr;
            if (typeDef->parentIndex != kInvalidIndex)
            {
                parentType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(typeDef->parentIndex);
                const Il2CppGenericContext* gc = type->type == IL2CPP_TYPE_GENERICINST ? &type->data.generic_class->context : nullptr;
                parentType = gc ? TryInflateIfNeed(parentType, gc, true) : parentType;
                CalcClassNotStaticFields(parentType);
                ClassLayoutInfo* parentLayout = GetClassLayoutInfo(parentType);
                IL2CPP_ASSERT(parentLayout);
                layout.instanceSize = parentLayout->instanceSize;
                layout.actualSize = parentLayout->actualSize;
                layout.alignment = parentLayout->alignment;
            }
            else
            {
                layout.instanceSize = sizeof(Il2CppObject);
                layout.actualSize = layout.instanceSize;
                layout.alignment = PTR_SIZE;
            }
            if (layoutClassSize > 0)
            {
                layout.instanceSize = std::max(layout.instanceSize, layoutClassSize + (int32_t)sizeof(Il2CppObject));
                layout.actualSize = layout.instanceSize;
            }
            layout.nativeSize = -1;
            layout.blittable = false;
            return;
        }

		const Il2CppGenericContext* gc = type->type == IL2CPP_TYPE_GENERICINST ? &type->data.generic_class->context : nullptr;
		for (uint16_t i = 0; i < typeDef->field_count; i++)
		{
			Il2CppFieldDefinition* fieldDef = (Il2CppFieldDefinition*)il2cpp::vm::GlobalMetadata::GetFieldDefinitionFromTypeDefAndFieldIndex(typeDef, i);
			const Il2CppType* fieldType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(fieldDef->typeIndex);
            const Il2CppType* inflatedFieldType = gc ? TryInflateIfNeed(fieldType, gc, true) : fieldType;
            FieldLayout& fieldLayout = fields[i];
            fieldLayout.type = inflatedFieldType;
            if (isCurAssemblyType)
            {
                int32_t offset = _image->GetFieldOffset(typeDef, i);
                fieldLayout.offset = offset;
                fieldLayout.isNormalStatic = IsRawNormalStaticField(inflatedFieldType, offset);
                fieldLayout.isThreadStatic = IsRawThreadStaticField(inflatedFieldType, offset);
            }
            else
            {

                Il2CppClass* klass = il2cpp::vm::GlobalMetadata::GetTypeInfoFromHandle((Il2CppMetadataTypeHandle)typeDef);
                il2cpp::vm::Class::SetupFields(klass);
                FieldInfo* fieldInfo = klass->fields + i;
                fieldLayout.offset = fieldInfo->offset;
                fieldLayout.isNormalStatic = il2cpp::vm::Field::IsNormalStatic(fieldInfo);
                fieldLayout.isThreadStatic = il2cpp::vm::Field::IsThreadStatic(fieldInfo);
            } 
			if (fieldLayout.isNormalStatic || fieldLayout.isThreadStatic)
			{
				layout.hasStaticFields = true;
			}
		}

        if (il2cpp::metadata::GenericMetadata::ContainsGenericParameters(type)
            || ((type->type == IL2CPP_TYPE_VALUETYPE || type->type == IL2CPP_TYPE_CLASS) && typeDef->genericContainerIndex != kGenericContainerIndexInvalid))
        {
            layout.instanceSize = 0;
            layout.actualSize = 0;
            layout.nativeSize = -1;
            layout.alignment = 1;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            layout.naturalAlignment = 1;
#endif
            layout.blittable = false;
			return;
        }

        uint8_t packingSize = 0;
        int32_t layoutClassSize = 0;
        if (IsInterpreterType(typeDef))
        {
            auto classLayoutData = MetadataModule::GetImage(typeDef)->GetClassLayout(typeDef);
            packingSize = (uint8_t)classLayoutData.packingSize;
            layoutClassSize = (int32_t)classLayoutData.classSize;
        }
        else
        {
#if HYBRIDCLR_UNITY_2020_OR_NEW
            packingSize = (uint8_t)il2cpp::vm::GlobalMetadata::StructLayoutPack((Il2CppMetadataTypeHandle)typeDef);
#else
            TypeDefinitionIndex typeIndex = il2cpp::vm::GlobalMetadata::GetTypeDefinitionIndexFromTypeDefinition(typeDef);
            packingSize = (uint8_t)il2cpp::vm::GlobalMetadata::StructLayoutPack(typeIndex);
#endif
        }
        int32_t classSizeWithHeader = layoutClassSize + sizeof(Il2CppObject);

		const bool isValueType = IsValueType(typeDef);
		bool blittable = isValueType;
		uint32_t instanceFieldCount = 0;
		for (FieldLayout& field : fields)
		{
			if (IsInstanceField(field.type))
			{
				++instanceFieldCount;
				// Reference-type layouts are never blittable. Avoid recursively
				// checking every field when only the value-type path consumes this bit.
				if (isValueType)
				{
					blittable &= IsBlittable(field.type);
				}
			}
		}
		layout.blittable = blittable;

		// If the type is not blittable, ignore packingSize
        if (!blittable)
        {
            packingSize = 0;
        }
		// packingSize is ignored for auto layout types
        if (!(typeDef->flags & (TYPE_ATTRIBUTE_SEQUENTIAL_LAYOUT | TYPE_ATTRIBUTE_EXPLICIT_LAYOUT)))
        {
            packingSize = 0;
        }

        if (typeDef->flags & TYPE_ATTRIBUTE_EXPLICIT_LAYOUT)
        {
            IL2CPP_ASSERT(IsValueType(typeDef));
            IL2CPP_ASSERT(isCurAssemblyType);
            int32_t instanceSize = IL2CPP_SIZEOF_STRUCT_WITH_NO_INSTANCE_FIELDS + sizeof(Il2CppObject);
            if (layoutClassSize > 0)
			{
                instanceSize = std::max(instanceSize, classSizeWithHeader);
			}
            int32_t maxAlignment = 1;
            int32_t nativeSize = 1;
            for (FieldLayout& field : fields)
            {
				if (!IsInstanceField(field.type))
				{
					continue;
				}
				SizeAndAlignment sa = GetTypeSizeAndAlignment(field.type);
				bool fieldBlittable = IsBlittable(field.type);
                if (!fieldBlittable && field.offset % PTR_SIZE != 0)
                {
                    const char* typeName = il2cpp::vm::GlobalMetadata::GetStringFromIndex(typeDef->nameIndex);
                    TEMP_FORMAT(errMsg, "Type %s is not blittable and has an invalid layout", typeName);
					RaiseExecutionEngineException(errMsg);
                }
				instanceSize = std::max(instanceSize, field.offset + (int32_t)sa.size);

                // compute size of alignment field
				uint8_t actualAlignment = packingSize != 0 ? std::min(packingSize, sa.alignment) : sa.alignment;
				instanceSize = std::max(instanceSize, AlignTo(field.offset, actualAlignment) + (int32_t)sa.size);

                maxAlignment = std::max(maxAlignment, (int32_t)sa.alignment);
                if (packingSize != 0)
                {
					maxAlignment = std::min(maxAlignment, (int32_t)packingSize);
				}
				nativeSize = AlignTo(std::max(nativeSize, field.offset + sa.nativeSize - (int32_t)sizeof(Il2CppObject)), maxAlignment);
            }
            layout.alignment = maxAlignment;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            // in unity 2021- version, il2cpp force alignment to 1 for explicit layout
            //layout.alignment = 1;
            //IL2CPP_ASSERT(blittable || layout.alignment == PTR_SIZE);
            layout.naturalAlignment = !blittable ? PTR_SIZE : layout.alignment;
#endif
            layout.actualSize = layout.instanceSize = AlignTo(instanceSize, layout.alignment);
            layout.nativeSize = nativeSize;
            if (layoutClassSize > 0)
            {
                layout.actualSize = std::max(layout.actualSize, classSizeWithHeader);
                layout.instanceSize = std::max(layout.instanceSize, classSizeWithHeader);
                layout.nativeSize = std::max((int32_t)layoutClassSize, layout.nativeSize);
            }
        }
        else
        {
            uint8_t parentMinimumAligment;
            int32_t parentActualSize = 0;
            bool isValueType = IsValueType(typeDef);
            if (typeDef->parentIndex != kInvalidIndex)
            {
                if (isValueType)
                {
                    parentMinimumAligment = 1;
                    parentActualSize = sizeof(Il2CppObject);
                }
                else
                {
                    const Il2CppType* parentType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(typeDef->parentIndex);
                    parentType = TryInflateIfNeed(parentType, gc, true);
                    CalcClassNotStaticFields(parentType);
                    ClassLayoutInfo* parentLayout = GetClassLayoutInfo(parentType);
                    parentActualSize = parentLayout->actualSize;
                    parentMinimumAligment = parentLayout->alignment;
                }
            }
            else
            {
                parentActualSize = sizeof(Il2CppObject);
                parentMinimumAligment = PTR_SIZE;
            }

            FieldLayoutData layoutData;
			LayoutFields(parentActualSize, parentMinimumAligment, packingSize, fields, FieldLayoutKind::Instance, layoutData);
			if (instanceFieldCount == 0 && isValueType)
            {
                layoutData.classSize = layoutData.actualClassSize = IL2CPP_SIZEOF_STRUCT_WITH_NO_INSTANCE_FIELDS + sizeof(Il2CppObject);
                layoutData.nativeSize = IL2CPP_SIZEOF_STRUCT_WITH_NO_INSTANCE_FIELDS;
            }
            layout.alignment = layoutData.minimumAlignment;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
            //layout.naturalAlignment = layoutData.naturalAlignment;
            layout.naturalAlignment = blittable ? layout.alignment : PTR_SIZE;
#endif
            layout.actualSize = layoutData.actualClassSize;
            layout.instanceSize = layoutData.classSize;
            layout.nativeSize = AlignTo(layoutData.nativeSize, layout.alignment);

            if (!isValueType)
            {
                layout.nativeSize = -1;
            }
            if (layoutClassSize > 0)
            {
                layout.actualSize = std::max(layout.actualSize, classSizeWithHeader);
                layout.instanceSize = std::max(layout.instanceSize, classSizeWithHeader);
                layout.nativeSize = isValueType ? std::max((int32_t)layoutClassSize, layout.nativeSize) : -1;
            }
        }
	}

	void ClassFieldLayoutCalculator::CalcClassStaticFields(const Il2CppType* type)
	{
		ClassLayoutInfo* layoutInfo = GetClassLayoutInfo(type);
		IL2CPP_ASSERT(layoutInfo);
		ClassLayoutInfo& layout = *layoutInfo;
		if (!layout.hasStaticFields)
		{
			return;
		}

		bool hasNormalStaticFields = false;
		bool hasThreadStaticFields = false;
        for (FieldLayout& field : layout.fields)
        {
            if (field.isNormalStatic)
            {
				hasNormalStaticFields = true;
            }
            else if (field.isThreadStatic)
            {
				hasThreadStaticFields = true;
            }
        }
		if (hasNormalStaticFields)
        {
            FieldLayoutData staticLayoutData;
			LayoutFields(0, 1, 0, layout.fields, FieldLayoutKind::Static, staticLayoutData);
            layout.staticFieldsSize = staticLayoutData.classSize;
        }
		if (hasThreadStaticFields)
        {
            FieldLayoutData threadStaticLayoutData;
			LayoutFields(0, 1, 0, layout.fields, FieldLayoutKind::ThreadStatic, threadStaticLayoutData);
            layout.threadStaticFieldsSize = threadStaticLayoutData.classSize;
			for (FieldLayout& field : layout.fields)
            {
				if (field.isThreadStatic)
				{
					field.offset = field.offset | THREAD_LOCAL_STATIC_MASK;
				}
            }
        }
	}
}
}
