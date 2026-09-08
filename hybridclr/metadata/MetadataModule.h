#pragma once

#include "InterpreterImage.h"
#include "AOTHomologousImage.h"
#include "Assembly.h"
#include "../DheRuntime.h"

namespace hybridclr
{

namespace metadata
{
	class MetadataModule
	{
	public:

		static void Initialize();

		static InterpreterImage* GetImage(uint32_t imageIndex)
		{
			return InterpreterImage::GetImage(imageIndex);
		}

		static InterpreterImage* GetImage(const Il2CppImage* image)
		{
			return GetImage(DecodeImageIndex(image->token));
		}

		static InterpreterImage* GetImage(const Il2CppClass* klass)
		{
			return GetImage(klass->image);
		}

		static InterpreterImage* GetImage(const Il2CppTypeDefinition* typeDef)
		{
			return GetImage(DecodeImageIndex(typeDef->byvalTypeIndex));
		}

		static InterpreterImage* GetImage(const Il2CppMethodDefinition* method)
		{
			return GetImage(DecodeImageIndex(method->nameIndex));
		}

		static InterpreterImage* GetImage(const MethodInfo* method)
		{
			return GetImage(method->klass->image);
		}

		static InterpreterImage* GetImageByEncodedIndex(uint32_t encodedIndex)
		{
			return GetImage(DecodeImageIndex(encodedIndex));
		}
		
		static Image* GetUnderlyingInterpreterImage(const MethodInfo* method);
		static Image* GetInterpreterResolveImage(const MethodInfo* method);

        static const Il2CppImage* GetDheMethodMetadataImage(const MethodInfo* method);
		static bool TryGetDheInterfaceInvokeData(const Il2CppClass* klass,
			const Il2CppClass* interfaceType, uint16_t logicalSlot, const VirtualInvokeData*& result);

        static bool TryGetDheReferencedAssemblies(const Il2CppAssembly* assembly,
            std::vector<const Il2CppAssemblyName*>& references);

		static Il2CppClass* FindDheSupplementalType(const Il2CppImage* image,
			const char* namespaze, const char* name);

		static void GetDheSupplementalTypes(const Il2CppImage* image,
			std::vector<const Il2CppClass*>& types);

		static Il2CppClass* GetFirstDheSupplementalNestedType(Il2CppClass* klass,
			void** iter);

		static bool TryGetNextDheSupplementalNestedType(Il2CppClass* klass, void** iter,
			Il2CppClass** nestedType);

		static const MethodInfo* GetFirstDheSupplementalMethod(Il2CppClass* klass,
			void** iter);

		static bool TryGetNextDheSupplementalMethod(Il2CppClass* klass, void** iter,
			const MethodInfo** method);

		static FieldInfo* GetFirstDheSupplementalField(Il2CppClass* klass, void** iter);

		static bool TryGetNextDheSupplementalField(Il2CppClass* klass, void** iter,
			FieldInfo** field);

		static size_t GetDheSupplementalFieldCount(Il2CppClass* klass);

		static const FieldInfo* ResolveDheSupplementalField(const FieldInfo* field);

		static const Il2CppFieldDefinition* ResolveDheSupplementalFieldDefinition(
			const Il2CppType* type, const char* name, const Il2CppType* fieldType);

		static bool IsDheRemovedMethod(const MethodInfo* method)
		{
			return dhe::IsRemovedMethod(method);
		}

		static size_t GetDheRemovedMethodCount(Il2CppClass* klass);

		static size_t GetDheSupplementalMethodCount(Il2CppClass* klass);

		static bool IsDheRemovedType(const Il2CppClass* klass)
		{
			return dhe::IsRemovedType(klass);
		}

		static bool IsDheRemovedField(const FieldInfo* field);

		static size_t GetDheRemovedFieldCount(Il2CppClass* klass);

		static Il2CppClass* GetDheLogicalFieldParent(FieldInfo* field);

		static void RegisterDheSupplementalInstanceField(FieldInfo* runtimeField,
			FieldInfo* logicalField, const FieldInfo* definitionField = nullptr);

		static bool IsDheSupplementalInstanceField(const FieldInfo* field);

		static bool TryGetDheSupplementalInstanceFieldValue(Il2CppObject* obj,
			FieldInfo* field, void* value);

		static bool TrySetDheSupplementalInstanceFieldValue(Il2CppObject* obj,
			const FieldInfo* field, void* value, bool dereferencePointer = false);

		static bool TryGetDheSupplementalInstanceFieldAddress(Il2CppObject* obj,
			const FieldInfo* field, void** address);

		static bool TryGetDheSupplementalInstanceFieldValueObject(Il2CppObject* obj,
			FieldInfo* field, Il2CppObject** value);

		static bool TrySetDheSupplementalInstanceFieldValueObject(Il2CppObject* obj,
			FieldInfo* field, Il2CppObject* value);

		static bool HasDheLogicalPropertyView(Il2CppClass* klass);

		static const PropertyInfo* GetFirstDheLogicalProperty(Il2CppClass* klass,
			void** iter);

		static bool TryGetNextDheLogicalProperty(Il2CppClass* klass, void** iter,
			const PropertyInfo** property);

		static size_t GetDheLogicalPropertyCount(Il2CppClass* klass);

		static const PropertyInfo* GetDheCustomAttributeProperty(Il2CppClass* klass, uint32_t index);

		static const MethodInfo* ResolveDheCustomAttributeConstructor(const MethodInfo* method);

		static bool HasDheLogicalEventView(Il2CppClass* klass);

		static const EventInfo* GetFirstDheLogicalEvent(Il2CppClass* klass,
			void** iter);

		static bool TryGetNextDheLogicalEvent(Il2CppClass* klass, void** iter,
			const EventInfo** eventInfo);

		static size_t GetDheLogicalEventCount(Il2CppClass* klass);


		static const char* GetStringFromEncodeIndex(StringIndex index)
		{
			uint32_t imageIndex = DecodeImageIndex(index);
			return GetImage(imageIndex)->GetStringFromRawIndex(DecodeMetadataIndex(index));
		}

		static uint32_t GetTypeEncodeIndex(const Il2CppTypeDefinition* typeDef)
		{
			InterpreterImage* image = GetImage(typeDef);
			return hybridclr::metadata::EncodeImageAndMetadataIndex(image->GetIndex(), image->GetTypeRawIndex(typeDef));
		}

		static Il2CppMetadataTypeHandle GetAssemblyTypeHandleFromRawIndex(const Il2CppImage* image, AssemblyTypeIndex index)
		{
			return GetImage(image)->GetAssemblyTypeHandleFromRawIndex(index);
		}

		static Il2CppMetadataTypeHandle GetAssemblyTypeHandleFromEncodeIndex(AssemblyTypeIndex index)
		{
			uint32_t imageIndex = DecodeImageIndex(index);
			return GetImage(imageIndex)->GetAssemblyTypeHandleFromRawIndex(DecodeMetadataIndex(index));
		}

		static Il2CppMetadataTypeHandle GetAssemblyExportedTypeHandleFromEncodeIndex(AssemblyTypeIndex index)
		{
			uint32_t imageIndex = DecodeImageIndex(index);
			return GetImage(imageIndex)->GetAssemblyExportedTypeHandleFromRawIndex(DecodeMetadataIndex(index));
		}

		static const Il2CppTypeDefinitionSizes* GetTypeDefinitionSizesFromEncodeIndex(TypeDefinitionIndex index)
		{
			uint32_t imageIndex = DecodeImageIndex(index);
			return GetImage(imageIndex)->GetTypeDefinitionSizesFromRawIndex(DecodeMetadataIndex(index));
		}

		static const Il2CppType* GetIl2CppTypeFromEncodeIndex(uint32_t index)
		{
			uint32_t imageIndex = DecodeImageIndex(index);
			IL2CPP_ASSERT(imageIndex > 0);

			uint32_t rawIndex = DecodeMetadataIndex(index);
			return GetImage(imageIndex)->GetIl2CppTypeFromRawIndex(rawIndex);
		}

		static Il2CppClass* GetTypeInfoFromTypeDefinitionEncodeIndex(TypeDefinitionIndex index)
		{
			uint32_t imageIndex = DecodeImageIndex(index);
			IL2CPP_ASSERT(imageIndex > 0);

			uint32_t rawIndex = DecodeMetadataIndex(index);
			return GetImage(imageIndex)->GetTypeInfoFromTypeDefinitionRawIndex(rawIndex);
		}

		static const Il2CppFieldDefinition* GetFieldDefinitionFromEncodeIndex(uint32_t index)
		{
			uint32_t imageIndex = DecodeImageIndex(index);
			return GetImage(imageIndex)->GetFieldDefinitionFromRawIndex(DecodeMetadataIndex(index));
		}

		static const Il2CppMethodDefinition* GetMethodDefinitionFromIndex(MethodIndex index)
		{
			uint32_t imageIndex = DecodeImageIndex(index);
			return GetImage(imageIndex)->GetMethodDefinitionFromRawIndex(DecodeMetadataIndex(index));
		}

		static void EnsureTypeMethodMetadataInitializedLocked(const Il2CppClass* klass)
		{
			IL2CPP_ASSERT(IsInterpreterType(klass));
			const Il2CppTypeDefinition* typeDef = reinterpret_cast<const Il2CppTypeDefinition*>(klass->typeMetadataHandle);
			GetImage(klass)->EnsureTypeMethodMetadataInitializedLocked(typeDef);
		}

		static void EnsureTypeFieldMetadataInitializedLocked(const Il2CppClass* klass)
		{
			IL2CPP_ASSERT(IsInterpreterType(klass));
			const Il2CppTypeDefinition* typeDef = reinterpret_cast<const Il2CppTypeDefinition*>(klass->typeMetadataHandle);
			GetImage(klass)->EnsureTypeFieldMetadataInitializedLocked(typeDef);
		}

		static bool TryApplyClassLayoutLocked(Il2CppClass* klass)
		{
			IL2CPP_ASSERT(IsInterpreterType(klass));
			return GetImage(klass)->TryApplyClassLayoutLocked(klass);
		}

		static uint32_t GetFieldOffset(const Il2CppClass* klass, int32_t fieldIndexInType, FieldInfo* field)
		{
			return GetImage(klass)->GetFieldOffset(klass, fieldIndexInType);
		}

		static const MethodInfo* GetMethodInfoFromMethodDefinitionIndex(uint32_t index)
		{
			uint32_t imageIndex = DecodeImageIndex(index);
			return GetImage(imageIndex)->GetMethodInfoFromMethodDefinitionRawIndex(DecodeMetadataIndex(index));
		}

		static const MethodInfo* GetMethodInfoFromMethodDefinition(const Il2CppMethodDefinition* methodDef)
		{
			uint32_t imageIndex = DecodeImageIndex(methodDef->nameIndex);
			return GetImage(imageIndex)->GetMethodInfoFromMethodDefinition(methodDef);
		}

		static const MethodInfo* GetMethodInfoFromVTableSlot(const Il2CppClass* klass, int32_t vTableSlot)
		{
			return GetImage(klass)->GetMethodInfoFromVTableSlot(klass, vTableSlot);
		}

		static const Il2CppMethodDefinition* GetMethodDefinitionFromVTableSlot(const Il2CppTypeDefinition* typeDefine, int32_t vTableSlot)
		{
			return GetImage(typeDefine)->GetMethodDefinitionFromVTableSlot(typeDefine, vTableSlot);
		}

		static Il2CppMethodPointer GetAdjustorThunk(const Il2CppImage* image, uint32_t token)
		{
			uint32_t imageIndex = DecodeImageIndex(image->token);
			return GetImage(imageIndex)->GetAdjustorThunk(token);
		}

		static Il2CppMethodPointer GetMethodPointer(const Il2CppImage* image, uint32_t token)
		{
			uint32_t imageIndex = DecodeImageIndex(image->token);
			return GetImage(imageIndex)->GetMethodPointer(token);
		}

		static InvokerMethod GetMethodInvoker(const Il2CppImage* image, uint32_t token)
		{
			uint32_t imageIndex = DecodeImageIndex(image->token);
			return GetImage(imageIndex)->GetMethodInvoker(token);
		}

		static const Il2CppParameterDefinition* GetParameterDefinitionFromIndex(const Il2CppImage* image, ParameterIndex index)
		{
			uint32_t imageIndex = DecodeImageIndex(image->token);
			return GetImage(imageIndex)->GetParameterDefinitionFromIndex(index);
		}

		static const Il2CppType* GetInterfaceFromIndex(const Il2CppClass* klass, TypeInterfaceIndex index)
		{
			return GetImage(klass)->GetInterfaceFromIndex(klass, index);
		}

		static const Il2CppType* GetInterfaceFromOffset(const Il2CppClass* klass, TypeInterfaceIndex offset)
		{
			return GetImage(klass)->GetInterfaceFromOffset(klass, offset);
		}

		static const Il2CppType* GetInterfaceFromOffset(const Il2CppTypeDefinition* typeDefine, TypeInterfaceIndex offset)
		{
			return GetImage(typeDefine)->GetInterfaceFromOffset(typeDefine, offset);
		}

		static Il2CppInterfaceOffsetInfo GetInterfaceOffsetInfo(const Il2CppTypeDefinition* typeDefine, TypeInterfaceOffsetIndex index)
		{
			return GetImage(typeDefine)->GetInterfaceOffsetInfo(typeDefine, index);
		}

		static Il2CppClass* GetNestedTypeFromOffset(const Il2CppClass* klass, TypeNestedTypeIndex offset)
		{
			return GetImage(klass)->GetNestedTypeFromOffset(klass, offset);
		}

		static Il2CppClass* GetNestedTypeFromOffset(const Il2CppTypeDefinition* typeDefinition, TypeNestedTypeIndex offset)
		{
			return GetImage(typeDefinition)->GetNestedTypeFromOffset(typeDefinition, offset);
		}

		static Il2CppMetadataTypeHandle GetNestedTypes(Il2CppMetadataTypeHandle handle, void** iter)
		{
			Il2CppTypeDefinition* typeDef = (Il2CppTypeDefinition*)handle;
			return (Il2CppMetadataTypeHandle)(GetImage(typeDef)->GetNestedTypes(typeDef, iter));
		}

		static const Il2CppGenericContainer* GetGenericContainerFromEncodeIndex(uint32_t index)
		{
			return GetImage(DecodeImageIndex(index))->GetGenericContainerByRawIndex(DecodeMetadataIndex(index));

		}

		static const Il2CppFieldDefaultValue* GetFieldDefaultValueEntry(uint32_t index)
		{
			return GetImage(DecodeImageIndex(index))->GetFieldDefaultValueEntryByRawIndex(DecodeMetadataIndex(index));
		}

		static const uint8_t* GetFieldOrParameterDefalutValue(uint32_t index)
		{
			return GetImage(DecodeImageIndex(index))->GetFieldOrParameterDefalutValueByRawIndex(DecodeMetadataIndex(index));
		}

		static bool TryGetDheCustomAttributeSource(const Il2CppImage* image,
			uint32_t token, const Il2CppImage*& sourceImage, uint32_t& sourceToken);

#if HYBRIDCLR_UNITY_2020
		static bool HasAttribute(const Il2CppImage* image, uint32_t token, Il2CppClass* attribute)
		{
			return GetImage(image)->HasAttributeByToken(token, attribute);
		}

		static std::tuple<void*, void*> GetCustomAttributeDataRange(const Il2CppImage* image, uint32_t token)
		{
			return GetImage(image)->GetCustomAttributeDataRange(token);
		}
#endif

		static bool IsImplementedByInterpreter(MethodInfo* method, bool aotImplementationMissing = false)
		{
			Il2CppClass* klass = method->klass;
			const Il2CppAssembly* assembly = klass && klass->image ? klass->image->assembly : nullptr;
			if (AOTHomologousImage* image = AOTHomologousImage::FindImageByAssembly(assembly))
			{
				// Ordinary supplemental metadata keeps the historical assembly-wide
				// interpreter behavior. A registered DHE image narrows it to the
				// methods listed by mv, unless the requested instantiation has no AOT code.
				if (dhe::IsDheAssembly(assembly))
				{
					// Supplemental aliases keep a Base declaring class, but have no
					// Base token or AOT implementation, including generic inflations.
					return aotImplementationMissing || IsInterpreterImplement(method) ||
						image->GetSupplementalMethodImage(method) != nullptr ||
						dhe::IsChangedMethod(method);
				}
				return true;
			}
			Il2CppClass* parent = klass->parent;
			if (parent != il2cpp_defaults.multicastdelegate_class && parent != il2cpp_defaults.delegate_class)
			{
				return AOTHomologousImage::FindImageByAssembly(klass->image->assembly);
			}
			else
			{
				return strcmp(method->name, "Invoke") == 0;
			}
		}
	private:

	};
}

}
