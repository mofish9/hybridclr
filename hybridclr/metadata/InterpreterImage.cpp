#include "InterpreterImage.h"
#include "SuperSetAOTHomologousImage.h"

#include <cstring>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <iostream>
#include <algorithm>
#include <limits>

#if defined(__has_include)
#if __has_include("../lab/InstrumentationConfig.h")
#include "../lab/InstrumentationConfig.h"
#endif
#endif

#include "il2cpp-class-internals.h"
#include "vm/GlobalMetadata.h"
#include "vm/Class.h"
#include "vm/Type.h"
#include "vm/Field.h"
#include "vm/Object.h"
#include "vm/Runtime.h"
#include "vm/Array.h"
#include "vm/MetadataLock.h"
#include "vm/MetadataCache.h"
#include "vm/MetadataAlloc.h"
#include "vm/String.h"
#include "vm/Reflection.h"
#include "metadata/FieldLayout.h"
#include "metadata/Il2CppTypeCompare.h"
#include "metadata/GenericMetadata.h"
#if HYBRIDCLR_UNITY_2021_OR_NEW
#include "metadata/CustomAttributeCreator.h"
#endif
#include "os/Atomic.h"
#include "icalls/mscorlib/System/MonoCustomAttrs.h"

#include "MetadataModule.h"
#include "MetadataUtil.h"
#include "ClassFieldLayoutCalculator.h"
#include "MetadataPool.h"

#include "../interpreter/Engine.h"
#include "../interpreter/InterpreterModule.h"

namespace hybridclr
{
namespace metadata
{
	// Il2CppTypeDefinition::bitfield stores the reflection-visible packing size
	// in bits 13-16 in both Unity 2021 and Unity/Tuanjie 2022 metadata layouts.
	static const uint32_t kSpecifiedPackingSizeBit = 13;

#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
	static FILE* s_metadataProfileFile = nullptr;
	static bool s_metadataProfileOpenAttempted = false;
	static std::mutex s_metadataProfileMutex;
	struct MetadataStageAggregate
	{
		uint32_t imageIndex;
		const char* stage;
		uint64_t elapsedNanoseconds;
	};
	static std::vector<MetadataStageAggregate> s_metadataStageAggregates;

	void RecordMetadataInitStage(uint32_t imageIndex, const char* stage, uint64_t elapsedNanoseconds)
	{
		std::lock_guard<std::mutex> lock(s_metadataProfileMutex);
		if (!s_metadataProfileOpenAttempted)
		{
			s_metadataProfileOpenAttempted = true;
			const char* path = std::getenv("HYBRIDCLR_METADATA_PROFILE");
			if (path && *path)
			{
				s_metadataProfileFile = std::fopen(path, "a");
				if (s_metadataProfileFile)
				{
					// Player shutdown may bypass the normal stdio flush path. The
					// instrumented profile is diagnostic-only, so keep each CSV row
					// durable as soon as it is recorded.
					std::setvbuf(s_metadataProfileFile, nullptr, _IONBF, 0);
				}
			}
		}
		if (!s_metadataProfileFile)
		{
			return;
		}
		std::fprintf(s_metadataProfileFile, "%u,%s,%llu\n", imageIndex, stage, (unsigned long long)elapsedNanoseconds);
	}

	void AccumulateMetadataInitStage(uint32_t imageIndex, const char* stage, uint64_t elapsedNanoseconds)
	{
		std::lock_guard<std::mutex> lock(s_metadataProfileMutex);
		if (!s_metadataProfileFile)
		{
			return;
		}
		for (MetadataStageAggregate& aggregate : s_metadataStageAggregates)
		{
			if (aggregate.imageIndex == imageIndex && !std::strcmp(aggregate.stage, stage))
			{
				aggregate.elapsedNanoseconds += elapsedNanoseconds;
				return;
			}
		}
		s_metadataStageAggregates.push_back({ imageIndex, stage, elapsedNanoseconds });
	}

	void FlushMetadataProfile()
	{
		for (uint32_t imageIndex = 0; imageIndex < kMaxMetadataImageCount; ++imageIndex)
		{
			InterpreterImage* image = InterpreterImage::GetImage(imageIndex);
			if (image)
			{
				image->FlushClassLayoutCacheInstrumentation();
			}
		}
		std::lock_guard<std::mutex> lock(s_metadataProfileMutex);
		if (s_metadataProfileFile)
		{
			for (const MetadataStageAggregate& aggregate : s_metadataStageAggregates)
			{
				std::fprintf(s_metadataProfileFile, "%u,%s,%llu\n", aggregate.imageIndex, aggregate.stage, (unsigned long long)aggregate.elapsedNanoseconds);
			}
			s_metadataStageAggregates.clear();
			std::fflush(s_metadataProfileFile);
		}
	}

#define HC_METADATA_STAGE(imageIndex, stageName, expression) \
	do \
	{ \
		auto _metadataStageStart = std::chrono::steady_clock::now(); \
		expression; \
		auto _metadataStageEnd = std::chrono::steady_clock::now(); \
		RecordMetadataInitStage((imageIndex), (stageName), (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(_metadataStageEnd - _metadataStageStart).count()); \
	} while (false)
#else
#define HC_METADATA_STAGE(imageIndex, stageName, expression) do { expression; } while (false)
#endif

	static uint32_t s_nextImageIndexByKind[4] = { (1u << kMetadataImageIndexExtraShiftBitsA), 0, 0, 0};

	static void ValidateCustomAttributeCodedIndexes(const RawImageBase& rawImage, uint32_t parent, uint32_t type)
	{
		const uint32_t parentTag = parent & 0x1f;
		if (parentTag > 21)
		{
			RaiseBadImageException("custom attribute parent coded index is invalid");
		}
		TableType parentTable = DecodeHasCustomAttributeCodedIndexTableType(parent);
		uint32_t parentRow = DecodeHasCustomAttributeCodedIndexRowIndex(parent);
		if (parentRow == 0 || parentRow > rawImage.GetTable(parentTable).rowNum)
		{
			RaiseBadImageException("custom attribute parent row index is out of range");
		}

		const uint32_t typeTag = type & 0x7;
		if (typeTag != 2 && typeTag != 3)
		{
			RaiseBadImageException("custom attribute constructor coded index is invalid");
		}
		TableType ctorTable = DecodeCustomAttributeTypeCodedIndexTableType(type);
		uint32_t ctorRow = DecodeCustomAttributeTypeCodedIndexRowIndex(type);
		if (ctorRow == 0 || ctorRow > rawImage.GetTable(ctorTable).rowNum)
		{
			RaiseBadImageException("custom attribute constructor row index is out of range");
		}
	}

	static TableType ValidateTypeDefOrRefOrSpecCodedIndex(const RawImageBase& rawImage, uint32_t codedIndex, const char* error)
	{
		if ((codedIndex & 0x3) == 0x3)
		{
			RaiseBadImageException(error);
		}
		TableType tableType = DecodeTypeDefOrRefOrSpecCodedIndexTableType(codedIndex);
		uint32_t rowIndex = DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(codedIndex);
		if (rowIndex == 0 || rowIndex > rawImage.GetTable(tableType).rowNum)
		{
			RaiseBadImageException(error);
		}
		return tableType;
	}

	static TableType ValidateMethodDefOrRefCodedIndex(const RawImageBase& rawImage, uint32_t codedIndex, const char* error)
	{
		TableType tableType = DecodeMethodDefOrRefCodedIndexTableType(codedIndex);
		uint32_t rowIndex = DecodeMethodDefOrRefCodedIndexRowIndex(codedIndex);
		if ((tableType != TableType::METHOD && tableType != TableType::MEMBERREF) ||
			rowIndex == 0 || rowIndex > rawImage.GetTable(tableType).rowNum)
		{
			RaiseBadImageException(error);
		}
		return tableType;
	}

	InterpreterImage* InterpreterImage::s_images[kMaxMetadataImageCount] = {};

	void InterpreterImage::FlushClassLayoutCacheInstrumentation()
	{
	#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		if (_classLayoutCalculator)
		{
			_classLayoutCalculator->FlushInstrumentation();
		}
	#endif
	}

	static int32_t GetImageKindByDllLength(uint32_t dllLength)
	{
		uint32_t maxPossibleIndexValue = dllLength * 4;
		for (int32_t i = 3; i >= 0; i--)
		{
			if (maxPossibleIndexValue <= kMetadataIndexMaskArr[i])
			{
				return i;
			}
		}
		return -1;
	}

	void InterpreterImage::Initialize()
	{
		
	}

	uint32_t InterpreterImage::AllocImageIndex(uint32_t dllLength)
	{
		int32_t kind = GetImageKindByDllLength(dllLength);
		if (kind < 0)
		{
			return kInvalidImageIndex;
		}
		for (int32_t finalKind = kind; finalKind >= 0; finalKind--)
		{
			uint32_t newImageIndex = s_nextImageIndexByKind[finalKind];
			// 255 is preserved for invalid image index when kind is 3
			if (newImageIndex >= kMaxMetadataImageIndexWithoutKind - (finalKind == 3))
			{
				continue;
			}
			s_nextImageIndexByKind[finalKind] += (1u << kMetadataImageIndexExtraShiftBitsArr[finalKind]);
			return newImageIndex | ((uint32_t)finalKind << (kMetadataImageIndexBits - kMetadataKindBits));
		}
		return kInvalidImageIndex;
	}

	void InterpreterImage::RegisterImage(InterpreterImage* image)
	{
		IL2CPP_ASSERT(image->GetIndex() > 0);
		il2cpp::os::Atomic::PublishPointer(&s_images[image->GetIndex()], image);
	}

	void InterpreterImage::InitBasic(Il2CppImage* image)
	{
		SetIl2CppImage(image);
		RegisterImage(this);
	}

	void InterpreterImage::BuildIl2CppAssembly(Il2CppAssembly* ass)
	{
		ass->token = EncodeToken(TableType::ASSEMBLY, 1);
		ass->referencedAssemblyStart = EncodeWithIndex(1);
		ass->referencedAssemblyCount = _rawImage->GetTableRowNum(TableType::ASSEMBLYREF);

		TbAssembly data = _rawImage->ReadAssembly(1);
		auto& aname = ass->aname;
		aname.hash_alg = data.hashAlgId;
		aname.major = data.majorVersion;
		aname.minor = data.minorVersion;
		aname.build = data.buildNumber;
		aname.revision = data.revisionNumber;
		aname.flags = data.flags;
		aname.public_key = _rawImage->GetBlobFromRawIndex(data.publicKey);
		aname.name = _rawImage->GetStringFromRawIndex(data.name);
		aname.culture = _rawImage->GetStringFromRawIndex(data.locale);
	}

	void InterpreterImage::BuildIl2CppImage(Il2CppImage* image2)
	{
		image2->typeCount = _rawImage->GetTableRowNum(TableType::TYPEDEF);
		image2->exportedTypeCount = _rawImage->GetTableRowNum(TableType::EXPORTEDTYPE);
		image2->customAttributeCount = _rawImage->GetTableRowNum(TableType::CUSTOMATTRIBUTE);

#if HYBRIDCLR_UNITY_2019
		image2->typeStart = EncodeWithIndex(0);
		image2->customAttributeStart = EncodeWithIndex(0);
		image2->entryPointIndex = EncodeWithIndexExcept0(_rawImage->GetEntryPointToken());
		image2->exportedTypeStart = EncodeWithIndex(0);
#else
		Il2CppImageGlobalMetadata* metadataImage = (Il2CppImageGlobalMetadata*)HYBRIDCLR_METADATA_MALLOC(sizeof(Il2CppImageGlobalMetadata));
		metadataImage->typeStart = EncodeWithIndex(0);
		metadataImage->customAttributeStart = EncodeWithIndex(0);
		metadataImage->entryPointIndex = EncodeWithIndexExcept0(_rawImage->GetEntryPointToken());
		metadataImage->exportedTypeStart = EncodeWithIndex(0);
		metadataImage->image = image2;
		image2->metadataHandle = reinterpret_cast<Il2CppMetadataImageHandle>(metadataImage);
#endif

		image2->nameToClassHashTable = nullptr;
		image2->codeGenModule = nullptr;

		image2->token = EncodeWithIndex(0); // TODO
		image2->dynamic = 0;
	}

	void InterpreterImage::InitRuntimeMetadatas()
	{
		IL2CPP_ASSERT(_rawImage->GetTable(TableType::EXPORTEDTYPE).rowNum == 0);

		HC_METADATA_STAGE(_index, "InitGenericParamDefs0", InitGenericParamDefs0());
		HC_METADATA_STAGE(_index, "InitTypeDefs_0", InitTypeDefs_0());
		// Bind TypeDef references before signatures, parents and layouts can
		// publish hidden Current classes as the types of public declarations.
		if (_homologousTypeReferenceImage)
		{
			_homologousTypeReferenceImage->InitTypeReferences();
		}
		HC_METADATA_STAGE(_index, "InitMethodDefs0", InitMethodDefs0());
		HC_METADATA_STAGE(_index, "InitGenericParamDefs", InitGenericParamDefs());
		HC_METADATA_STAGE(_index, "InitNestedClass", InitNestedClass()); // must before typedefs1, because parent may be nested class
		HC_METADATA_STAGE(_index, "InitTypeDefs_1", InitTypeDefs_1());

		HC_METADATA_STAGE(_index, "InitGenericParamConstraintDefs", InitGenericParamConstraintDefs());

		HC_METADATA_STAGE(_index, "InitParamDefs", InitParamDefs());
		HC_METADATA_STAGE(_index, "InitMethodDefs", InitMethodDefs());
		HC_METADATA_STAGE(_index, "InitFieldDefs", InitFieldDefs());
		HC_METADATA_STAGE(_index, "InitFieldLayouts", InitFieldLayouts());
		HC_METADATA_STAGE(_index, "InitFieldRVAs", InitFieldRVAs());
		HC_METADATA_STAGE(_index, "InitMethodImpls0", InitMethodImpls0());
		HC_METADATA_STAGE(_index, "InitProperties", InitProperties());
		HC_METADATA_STAGE(_index, "InitEvents", InitEvents());
		HC_METADATA_STAGE(_index, "InitConsts", InitConsts());
		HC_METADATA_STAGE(_index, "InitCustomAttributes", InitCustomAttributes());
		HC_METADATA_STAGE(_index, "InitModuleRefs", InitModuleRefs());
		HC_METADATA_STAGE(_index, "InitImplMaps", InitImplMaps());
		HC_METADATA_STAGE(_index, "InitClassLayouts0", InitClassLayouts0());
		HC_METADATA_STAGE(_index, "InitHasFinalizers", InitHasFinalizers());
		HC_METADATA_STAGE(_index, "InitTypeDefs_2", InitTypeDefs_2());
		HC_METADATA_STAGE(_index, "InitClassLayouts", InitClassLayoutsLazy());
		HC_METADATA_STAGE(_index, "InitInterfaces", InitInterfaces());
		HC_METADATA_STAGE(_index, "InitClass", InitClass());
		HC_METADATA_STAGE(_index, "InitVTables", InitVTables());

		FreezeIl2CppTypeCache();

	}

#undef HC_METADATA_STAGE

	void InterpreterImage::InitTypeDefs_0()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		_typesDefines.resize(typeDefTb.rowNum);
		_typeDetails.resize(typeDefTb.rowNum);
		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& cur = _typesDefines[i];
			TypeDefinitionDetail& typeDetail = _typeDetails[i];
			typeDetail.typeSizes = {};

			uint32_t rowIndex = i + 1;
			TbTypeDef data = _rawImage->ReadTypeDef(rowIndex);

			cur = {};

			cur.genericContainerIndex = kGenericContainerIndexInvalid;
			cur.declaringTypeIndex = kTypeDefinitionIndexInvalid;
			cur.elementTypeIndex = kTypeDefinitionIndexInvalid;

			cur.token = EncodeToken(TableType::TYPEDEF, rowIndex);

			bool isValueType = data.extends && IsValueTypeFromToken(DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.extends), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.extends));
			Il2CppType* cppType = MetadataMallocT<Il2CppType>();
			cppType->type = isValueType ? IL2CPP_TYPE_VALUETYPE : IL2CPP_TYPE_CLASS;
			SET_IL2CPPTYPE_VALUE_TYPE(*cppType, isValueType);
			cppType->data.typeHandle = (Il2CppMetadataTypeHandle)&cur;
			cur.byvalTypeIndex = AddIl2CppTypeCache(cppType);
#if HYBRIDCLR_UNITY_2019
			Il2CppType* byRefType = MetadataMallocT<Il2CppType>();
			*byRefType = *cppType;
			byRefType->byref = 1;
			cur.byrefTypeIndex = AddIl2CppTypeCache(byRefType);
#endif

			// The first TypeDef is <Module>, which is not returned by Assembly.GetTypes().
			if (rowIndex == 1 || IsInterface(cur.flags))
			{
				cur.interfaceOffsetsStart = EncodeWithIndex(0);
				cur.interface_offsets_count = 0;
				cur.vtableStart = EncodeWithIndex(0);
				cur.vtable_count = 0;
			}
			else
			{
				cur.interfaceOffsetsStart = 0;
				cur.interface_offsets_count = 0;
				cur.vtableStart = 0;
				cur.vtable_count = 0;
			}
		}
	}

	void InterpreterImage::InitTypeDefs_1()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		const Table& typeRefTb = _rawImage->GetTable(TableType::TYPEREF);
		const uint32_t fieldRowCount = _rawImage->GetTableRowNum(TableType::FIELD);
		const uint32_t methodRowCount = _rawImage->GetTableRowNum(TableType::METHOD);
		// TypeRef resolution is independent of the declaring type's generic
		// context. Cache it for the common case where many types inherit from
		// the same external base (usually System.Object). TypeSpec is deliberately
		// left uncached because its result may depend on the current generic
		// container.
		std::vector<const Il2CppType*> typeRefCache(typeRefTb.rowNum + 1, nullptr);
		const Il2CppType* classifiedParent = nullptr;
		bool classifiedParentIsEnum = false;
		bool classifiedParentIsValueType = false;
		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& last = _typesDefines[i > 0 ? i - 1 : 0];
			Il2CppTypeDefinition& cur = _typesDefines[i];
			uint32_t rowIndex = i + 1;
			TbTypeDef data = _rawImage->ReadTypeDef(rowIndex); // token from 1

			cur.flags = data.flags;
			cur.nameIndex = EncodeWithIndex(data.typeName);
			cur.namespaceIndex = EncodeWithIndex(data.typeNamespace);

			if (data.fieldList == 0 || data.fieldList - 1 > fieldRowCount ||
				data.methodList == 0 || data.methodList - 1 > methodRowCount)
			{
				RaiseBadImageException("type field or method list index is out of range");
			}
			const uint32_t rawFieldStart = data.fieldList - 1;
			const uint32_t rawMethodStart = data.methodList - 1;
			if (i == 0 && (rawFieldStart != 0 || rawMethodStart != 0))
			{
				RaiseBadImageException("type field or method ownership does not start at the first row");
			}
			cur.fieldStart = EncodeWithIndex(rawFieldStart);
			cur.methodStart = EncodeWithIndex(rawMethodStart);

			if (i > 0)
			{
				const uint32_t lastFieldStart = DecodeMetadataIndex(last.fieldStart);
				const uint32_t lastMethodStart = DecodeMetadataIndex(last.methodStart);
				if (rawFieldStart < lastFieldStart || rawMethodStart < lastMethodStart)
				{
					RaiseBadImageException("type field or method list is not monotonic");
				}
				const uint32_t fieldCount = rawFieldStart - lastFieldStart;
				const uint32_t methodCount = rawMethodStart - lastMethodStart;
				if (fieldCount > std::numeric_limits<uint16_t>::max() || methodCount > std::numeric_limits<uint16_t>::max())
				{
					RaiseBadImageException("type field or method count exceeds runtime limits");
				}
				last.field_count = static_cast<uint16_t>(fieldCount);
				last.method_count = static_cast<uint16_t>(methodCount);
			}
			if (i == n - 1)
			{
				const uint32_t fieldCount = fieldRowCount - rawFieldStart;
				const uint32_t methodCount = methodRowCount - rawMethodStart;
				if (fieldCount > std::numeric_limits<uint16_t>::max() || methodCount > std::numeric_limits<uint16_t>::max())
				{
					RaiseBadImageException("type field or method count exceeds runtime limits");
				}
				cur.field_count = static_cast<uint16_t>(fieldCount);
				cur.method_count = static_cast<uint16_t>(methodCount);
			}

			if (data.extends != 0)
			{
				TableType parentTableType = ValidateTypeDefOrRefOrSpecCodedIndex(*_rawImage, data.extends,
					"type parent coded index is invalid");
				uint32_t parentRowIndex = DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.extends);
				const Il2CppType* parentType = nullptr;
				if (parentTableType == TableType::TYPEREF && parentRowIndex < typeRefCache.size())
				{
					parentType = typeRefCache[parentRowIndex];
					if (!parentType)
					{
						parentType = ReadTypeFromToken(GetGenericContainerByTypeDefinition(&cur), nullptr, parentTableType, parentRowIndex);
						typeRefCache[parentRowIndex] = parentType;
					}
				}
				else
				{
					parentType = ReadTypeFromToken(GetGenericContainerByTypeDefinition(&cur), nullptr, parentTableType, parentRowIndex);
				}

				if (parentType->type == IL2CPP_TYPE_CLASS || parentType->type == IL2CPP_TYPE_VALUETYPE)
				{
					if (parentType != classifiedParent)
					{
						classifiedParent = parentType;
						if (parentTableType == TableType::TYPEDEF &&
							IsValueTypeFromToken(parentTableType, parentRowIndex))
						{
							TbTypeDef parentDefinition = _rawImage->ReadTypeDef(parentRowIndex);
							const char* parentName = _rawImage->GetStringFromRawIndex(
								parentDefinition.typeName);
							classifiedParentIsEnum = std::strcmp(parentName, "Enum") == 0;
							classifiedParentIsValueType = true;
						}
						else
						{
							const Il2CppMetadataTypeHandle parentHandle = parentType->data.typeHandle;
							classifiedParentIsEnum = parentHandle ==
								il2cpp_defaults.enum_class->typeMetadataHandle;
							classifiedParentIsValueType = classifiedParentIsEnum ||
								parentHandle == il2cpp_defaults.value_type_class->typeMetadataHandle;
						}
					}
					if (classifiedParentIsEnum)
					{
						cur.bitfield |= (1 << (il2cpp::vm::kBitIsValueType - 1));
						cur.bitfield |= (1 << (il2cpp::vm::kBitIsEnum - 1));
					}
					else if (classifiedParentIsValueType)
					{
						cur.bitfield |= (1 << (il2cpp::vm::kBitIsValueType - 1));
					}
				}
				cur.parentIndex = AddIl2CppTypeCache(parentType);
			}
			else
			{
				cur.parentIndex = kInvalidIndex;
			}

			cur.elementTypeIndex = kInvalidIndex;
		}
	}

	void InterpreterImage::ComputeHasFinalizer(Il2CppTypeDefinition* def, std::vector<bool>& computFlags)
	{
		if (DecodeImageIndex(def->byvalTypeIndex) != GetIndex())
		{
			return;
		}
		uint32_t typeIndex = GetTypeRawIndex(def);
		if (computFlags[typeIndex])
		{
			return;
		}
		computFlags[typeIndex] = true;
		if (def->bitfield & (1 << (il2cpp::vm::kBitHasFinalizer - 1)))
		{
			return;
		}
		if (def->parentIndex != kInvalidIndex)
		{
			const Il2CppType* parentType = GetIl2CppTypeFromRawIndex(DecodeMetadataIndex(def->parentIndex));
			const Il2CppTypeDefinition* parentDef = metadata::GetUnderlyingTypeDefinition(parentType);
			ComputeHasFinalizer(const_cast<Il2CppTypeDefinition*>(parentDef), computFlags);
			if (parentDef->bitfield & (1 << (il2cpp::vm::kBitHasFinalizer - 1)))
			{
				def->bitfield |= (1 << (il2cpp::vm::kBitHasFinalizer - 1));
			}
		}
	}

	void InterpreterImage::InitHasFinalizers()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		std::vector<bool> computFlags(typeDefTb.rowNum, false);
		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& cur = _typesDefines[i];
			ComputeHasFinalizer(&cur, computFlags);
		}
	}

	void InterpreterImage::InitTypeDefs_2()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			TbTypeDef data = _rawImage->ReadTypeDef(i + 1); // token from 1

			Il2CppTypeDefinition& last = _typesDefines[i > 0 ? i - 1 : 0];
			Il2CppTypeDefinition& cur = _typesDefines[i];
			uint32_t typeIndex = i; // type index start from 0, diff with field index ...

			// enum element_type == 
			if (IsEnumType(&cur))
			{
				if (cur.field_count == 0)
				{
					RaiseBadImageException("enum type has no underlying value field");
				}
				EnsureFieldMetadataInitializedLocked(DecodeMetadataIndex(cur.fieldStart));
				cur.elementTypeIndex = _fieldDetails[DecodeMetadataIndex(cur.fieldStart)].fieldDef.typeIndex;
			}

			auto classLayoutRow = _classLayouts.find(typeIndex);
			uint16_t packingSize = 0;
			if (classLayoutRow != _classLayouts.end())
			{
				auto& layoutData = classLayoutRow->second;
				packingSize = layoutData.packingSize;
			}
			else
			{
				cur.bitfield |= (1 << (il2cpp::vm::kClassSizeIsDefault - 1));
			}
			if (packingSize != 0)
			{
				il2cpp::vm::PackingSize packingSizeEnum = il2cpp::vm::GlobalMetadata::ConvertPackingSizeToEnum((uint8_t)packingSize);
				cur.bitfield |= ((uint32_t)packingSizeEnum << (il2cpp::vm::kPackingSize - 1));
				cur.bitfield |= ((uint32_t)packingSizeEnum << (kSpecifiedPackingSizeBit - 1));
			}
			else
			{
				cur.bitfield |= (1 << (il2cpp::vm::kPackingSizeIsDefault - 1));
			}
		}
	}

	void InterpreterImage::InitParamDefs()
	{
	}


	void InterpreterImage::InitFieldDefs()
	{
		const Table& fieldTb = _rawImage->GetTable(TableType::FIELD);
		_fieldDetails.resize(fieldTb.rowNum);

		for (size_t i = 0; i < _typesDefines.size(); i++)
		{
			Il2CppTypeDefinition& typeDef = _typesDefines[i];
			uint32_t start = DecodeMetadataIndex(typeDef.fieldStart);
			for (uint32_t k = 0; k < typeDef.field_count; k++)
			{
				FieldDetail& fd = _fieldDetails[start + k];
				fd.typeDefIndex = (uint32_t)i;
			}
		}

		for (uint32_t i = 0, n = fieldTb.rowNum; i < n; i++)
		{
			FieldDetail& fd = _fieldDetails[i];
			Il2CppFieldDefinition& cur = fd.fieldDef;

			fd.offset = 0;
			fd.defaultValueIndex = kDefaultValueIndexNull;

			uint32_t rowIndex = i + 1;
			TbField data = _rawImage->ReadField(rowIndex);

			cur.nameIndex = EncodeWithIndex(data.name);
			cur.typeIndex = kTypeIndexInvalid;
			cur.token = EncodeToken(TableType::FIELD, rowIndex);
		}
	}

	void InterpreterImage::InitFieldLayouts()
	{
		const Table& tb = _rawImage->GetTable(TableType::FIELDLAYOUT);
		uint32_t lastField = 0;
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			TbFieldLayout data = _rawImage->ReadFieldLayout(i + 1);
			if (data.field == 0 || data.field > _fieldDetails.size())
			{
				RaiseBadImageException("field layout row index is out of range");
			}
			if (data.field <= lastField)
			{
				RaiseBadImageException("field layout rows are duplicated or not sorted");
			}
			if (data.offset > std::numeric_limits<uint32_t>::max() - sizeof(Il2CppObject))
			{
				RaiseBadImageException("field layout offset exceeds runtime limits");
			}
			_fieldDetails[data.field - 1].offset = sizeof(Il2CppObject) + data.offset;
			lastField = data.field;
		}
	}

	void InterpreterImage::InitFieldRVAs()
	{
		const Table& tb = _rawImage->GetTable(TableType::FIELDRVA);
		uint32_t lastField = 0;
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			TbFieldRVA data = _rawImage->ReadFieldRVA(i + 1);
			if (data.field == 0 || data.field > _fieldDetails.size())
			{
				RaiseBadImageException("field RVA row index is out of range");
			}
			if (data.field <= lastField)
			{
				RaiseBadImageException("field RVA rows are duplicated or not sorted");
			}
			EnsureFieldMetadataInitializedLocked(data.field - 1);
			FieldDetail& fd = _fieldDetails[data.field - 1];
			fd.defaultValueIndex = (uint32_t)_fieldDefaultValues.size();

			Il2CppFieldDefaultValue fdv = {};
			fdv.fieldIndex = data.field - 1;
			fdv.typeIndex = fd.fieldDef.typeIndex;

			uint32_t dataImageOffset = (uint32_t)-1;
			if (!_rawImage->TranslateRVAToImageOffset(data.rva, dataImageOffset))
			{
				RaiseBadImageException("field RVA is outside the image");
			}
#if HYBRIDCLR_UNITY_2021_OR_NEW
			fdv.dataIndex = (DefaultValueDataIndex)EncodeWithIndex(EncodeWithBlobSource(dataImageOffset, BlobSource::RAW_IMAGE));
#else
			fdv.dataIndex = (DefaultValueDataIndex)EncodeWithIndex(dataImageOffset);
#endif
			_fieldDefaultValues.push_back(fdv);
			lastField = data.field;
		}
	}

#if HYBRIDCLR_UNITY_2021_OR_NEW
	DefaultValueDataIndex InterpreterImage::ConvertConstValue(CustomAttributeDataWriter& writer, uint32_t blobIndex, const Il2CppType* type)
	{
		Il2CppTypeEnum ttype = type->type;
		if (ttype == IL2CPP_TYPE_CLASS)
		{
			return kDefaultValueIndexNull;
		}

		DefaultValueIndex retIndex = EncodeWithIndex(EncodeWithBlobSource((DefaultValueIndex)writer.Size(), BlobSource::CONVERTED_IL2CPP_FORMAT));

		BlobReader reader = _rawImage->GetBlobReaderByRawIndex(blobIndex);
		switch (type->type)
		{
		case IL2CPP_TYPE_BOOLEAN:
		case IL2CPP_TYPE_I1:
		case IL2CPP_TYPE_U1:
		{
			writer.Write(reader, 1);
			break;
		}
		case IL2CPP_TYPE_CHAR:
		case IL2CPP_TYPE_I2:
		case IL2CPP_TYPE_U2:
		{
			writer.Write(reader, 2);
			break;
		}
		case IL2CPP_TYPE_I4:
		{
			writer.WriteCompressedInt32((int32_t)reader.Read32());
			break;
		}
		case IL2CPP_TYPE_U4:
		{
			writer.WriteCompressedUint32(reader.Read32());
			break;
		}
		case IL2CPP_TYPE_R4:
		{
			writer.Write(reader, 4);
			break;
		}
		case IL2CPP_TYPE_I8:
		case IL2CPP_TYPE_U8:
		case IL2CPP_TYPE_R8:
		{
			writer.Write(reader, 8);
			break;
		}
		case IL2CPP_TYPE_STRING:
		{
			std::string str = il2cpp::utils::StringUtils::Utf16ToUtf8((const Il2CppChar*)reader.GetData(), reader.GetLength() / 2);
			writer.WriteCompressedInt32((int32_t)str.length());
			writer.WriteBytes((const uint8_t*)str.c_str(), (int32_t)str.length());
			break;
		}
		default:
		{
			RaiseExecutionEngineException("not supported const type");
		}
		}
		
		return retIndex;
	}
#endif

	void InterpreterImage::InitConsts()
	{
		const Table& tb = _rawImage->GetTable(TableType::CONSTANT);
		uint32_t lastParent = 0;
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			TbConstant data = _rawImage->ReadConstant(i + 1);
			if ((data.parent & 0x3) == 0x3 || data.parent <= lastParent)
			{
				RaiseBadImageException("constant parents are invalid, duplicated, or not sorted");
			}
			TableType parentType = DecodeHasConstantType(data.parent);
			uint32_t rowIndex = DecodeHashConstantIndex(data.parent);
			if (rowIndex == 0 || rowIndex > _rawImage->GetTable(parentType).rowNum)
			{
				RaiseBadImageException("constant parent row index is out of range");
			}

			Il2CppType tempType = {};
			tempType.type = (Il2CppTypeEnum)data.type;
			const Il2CppType& type = *MetadataPool::GetPooledIl2CppType(tempType);
			TypeIndex dataTypeIndex = AddIl2CppTypeCache(&type);
#if !HYBRIDCLR_UNITY_2021_OR_NEW
			bool isNullValue = type.type == IL2CPP_TYPE_CLASS;
#endif
			switch (parentType)
			{
			case TableType::FIELD:
			{
				FieldDetail& fd = _fieldDetails[rowIndex - 1];
				fd.defaultValueIndex = (uint32_t)_fieldDefaultValues.size();

				Il2CppFieldDefaultValue fdv = {};
				fdv.fieldIndex = rowIndex - 1;
				fdv.typeIndex = dataTypeIndex;
#if HYBRIDCLR_UNITY_2021_OR_NEW
				fdv.dataIndex = ConvertConstValue(_constValues, data.value, &type);
#else
				uint32_t dataImageOffset = _rawImage->GetImageOffsetOfBlob(type.type, data.value);
				fdv.dataIndex = isNullValue ? kDefaultValueIndexNull : (DefaultValueDataIndex)EncodeWithIndex(dataImageOffset);
#endif
				_fieldDefaultValues.push_back(fdv);
				break;
			}
			case TableType::PARAM:
			{
				Il2CppParameterDefaultValue pdv = {};
				pdv.typeIndex = dataTypeIndex;
				TbParam paramData = _rawImage->ReadParam(rowIndex);
				pdv.parameterIndex = paramData.sequence > 0 ? paramData.sequence - 1 : 0;
#if HYBRIDCLR_UNITY_2021_OR_NEW
				pdv.dataIndex = ConvertConstValue(_constValues, data.value, &type);
#else
				uint32_t dataImageOffset = _rawImage->GetImageOffsetOfBlob(type.type, data.value);
				pdv.dataIndex = isNullValue ? kDefaultValueIndexNull : (DefaultValueDataIndex)EncodeWithIndex(dataImageOffset);
#endif
				uint32_t defaultValueIndex = (uint32_t)_paramDefaultValues.size();
				_paramDefaultValues.push_back(pdv);
				_rawParamDefaultValueIndexes.emplace(rowIndex - 1, defaultValueIndex);
				break;
			}
			case TableType::PROPERTY:
			{
				RaiseNotSupportedException("not support property const");
				break;
			}
			default:
			{
				RaiseExecutionEngineException("not support const TableType");
				break;
			}
			}
			lastParent = data.parent;
		}
	}

	void InterpreterImage::InitCustomAttributes()
	{
	}

	void InterpreterImage::InitThreadStaticFields()
	{
		// Most hot-update assemblies have no ThreadStaticAttribute at all. The
		// custom-attribute table can be very large, so first inspect the much
		// smaller MemberRef table for the attribute constructor. This keeps the
		// negative case out of the first field layout/entry path while preserving
		// the existing scan for assemblies that can actually contain the marker.
		const Table& memberRefTable = _rawImage->GetTable(TableType::MEMBERREF);
		bool hasThreadStaticCtor = false;
		for (uint32_t rowIndex = 1; rowIndex <= memberRefTable.rowNum; ++rowIndex)
		{
			if (IsThreadStaticCtorToken(TableType::MEMBERREF, rowIndex))
			{
				hasThreadStaticCtor = true;
				break;
			}
		}
		if (!hasThreadStaticCtor)
		{
			_customAttributeRangeCount = 0;
			return;
		}

		_customAttributeRangeCount = 0;
		uint32_t previousParent = 0;
		_rawImage->VisitCustomAttributeParentAndType([this, &previousParent](uint32_t, uint32_t parent, uint32_t type)
		{
			ValidateCustomAttributeCodedIndexes(*_rawImage, parent, type);
			if (_customAttributeRangeCount == 0 || parent != previousParent)
			{
				++_customAttributeRangeCount;
				previousParent = parent;
			}
			if (DecodeHasCustomAttributeCodedIndexTableType(parent) == TableType::FIELD)
			{
				TableType ctorMethodTableType = DecodeCustomAttributeTypeCodedIndexTableType(type);
				uint32_t ctorMethodRowIndex = DecodeCustomAttributeTypeCodedIndexRowIndex(type);
				if (IsThreadStaticCtorToken(ctorMethodTableType, ctorMethodRowIndex))
				{
					uint32_t parentRowIndex = DecodeHasCustomAttributeCodedIndexRowIndex(parent);
					if (parentRowIndex == 0 || parentRowIndex > _fieldDetails.size())
					{
						RaiseBadImageException("thread static custom attribute field index is invalid");
					}
					_fieldDetails[parentRowIndex - 1].offset = THREAD_LOCAL_STATIC_MASK;
				}
			}
		});
	}

	void InterpreterImage::EnsureThreadStaticFieldsInitializedLocked()
	{
		if (_threadStaticFieldsInitialized)
		{
			return;
		}
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		auto stageStart = std::chrono::steady_clock::now();
#endif
		InitThreadStaticFields();
		_threadStaticFieldsInitialized = true;
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		RecordMetadataInitStage(_index, "LazyThreadStaticFields", (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stageStart).count());
#endif
	}

	void InterpreterImage::EnsureCustomAttributesInitialized()
	{
		if (IsMetadataPublished(&_customAttributesInitialized))
		{
			return;
		}
		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		if (IsMetadataPublished(&_customAttributesInitialized))
		{
			return;
		}
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		auto stageStart = std::chrono::steady_clock::now();
#endif
		BuildCustomAttributeIndexes();
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		RecordMetadataInitStage(_index, "LazyCustomAttributeIndexes", (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stageStart).count());
#endif
		PublishMetadata(&_customAttributesInitialized);
	}

	void InterpreterImage::BuildCustomAttributeIndexes()
	{
		EnsureThreadStaticFieldsInitializedLocked();
		const Table& tb = _rawImage->GetTable(TableType::CUSTOMATTRIBUTE);
		// If no ThreadStatic constructor exists, InitThreadStaticFields avoids
		// scanning the table and therefore cannot count distinct parents. The row
		// count is a conservative capacity for the deferred index and is paid only
		// when custom attributes are actually requested.
		uint32_t rangeCapacity = _customAttributeRangeCount != 0 ? _customAttributeRangeCount : tb.rowNum;
		_tokenCustomAttributes.Reset(rangeCapacity);
		_customAttributeInfos.reserve(rangeCapacity);
		_customAttributeHandles.reserve(rangeCapacity + 1);
		_customAttribues.reserve(tb.rowNum);

		Il2CppCustomAttributeTypeRange* curTypeRange = nullptr;
		_rawImage->VisitCustomAttributes([this, &curTypeRange](uint32_t, uint32_t parent, uint32_t type, uint32_t value)
		{
			ValidateCustomAttributeCodedIndexes(*_rawImage, parent, type);
			TableType parentType = DecodeHasCustomAttributeCodedIndexTableType(parent);
			uint32_t parentRowIndex = DecodeHasCustomAttributeCodedIndexRowIndex(parent);
			uint32_t token = EncodeToken(parentType, parentRowIndex);
			if (curTypeRange == nullptr || curTypeRange->token != token)
			{
				uint32_t existingHandleIndex;
				if (_tokenCustomAttributes.TryGet(token, existingHandleIndex))
				{
					RaiseBadImageException("custom attributes for one parent are not contiguous");
				}
				int32_t attributeStartIndex = EncodeWithIndex((int32_t)_customAttribues.size());
				uint32_t handleIndex = (uint32_t)_customAttributeHandles.size();
				_tokenCustomAttributes.Insert(token, handleIndex);
				_customAttributeInfos.push_back({ (int32_t)EncodeWithIndex(handleIndex), false, nullptr, nullptr });
#ifdef HYBRIDCLR_UNITY_2021_OR_NEW
				_customAttributeHandles.push_back({ token, (uint32_t)attributeStartIndex });
#else
				_customAttributeHandles.push_back({ token, attributeStartIndex, 0 });
#endif
				curTypeRange = &_customAttributeHandles[handleIndex];
			}
#if !HYBRIDCLR_UNITY_2021_OR_NEW
			++curTypeRange->count;
#endif
			TableType ctorMethodTableType = DecodeCustomAttributeTypeCodedIndexTableType(type);
			uint32_t ctorMethodRowIndex = DecodeCustomAttributeTypeCodedIndexRowIndex(type);
			uint32_t ctorMethodToken = EncodeToken(ctorMethodTableType, ctorMethodRowIndex);
			_customAttribues.push_back({ ctorMethodToken, value });
		});
		IL2CPP_ASSERT(_tokenCustomAttributes.Size() == _customAttributeHandles.size()
			&& _customAttributeInfos.size() == _customAttributeHandles.size());
#ifdef HYBRIDCLR_UNITY_2021_OR_NEW
		// add extra Il2CppCustomAttributeTypeRange for compute count
		_customAttributeHandles.push_back({ 0, EncodeWithIndex((int32_t)_customAttribues.size()) });
#endif
#if !HYBRIDCLR_UNITY_2022_OR_NEW
		_customAttribtesCaches.resize(_tokenCustomAttributes.Size());
#endif
	}

#ifdef HYBRIDCLR_UNITY_2021_OR_NEW

	void InterpreterImage::InitCustomAttributeData(CustomAttributesInfo& cai, const Il2CppCustomAttributeTypeRange& dataRange)
	{
		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		if (IsMetadataPublished(&cai.inited))
		{
			return;
		}
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		auto stageStart = std::chrono::steady_clock::now();
#endif
		BuildCustomAttributesData(cai, dataRange);
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		AccumulateMetadataInitStage(_index, "LazyCustomAttributeData", (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stageStart).count());
#endif
		PublishMetadata(&cai.inited);
	}

	const CustomAttributeCtorInfo& InterpreterImage::GetOrCreateCustomAttributeCtorInfo(uint32_t ctorMethodToken)
	{
		auto it = _customAttributeCtorInfos.find(ctorMethodToken);
		if (it != _customAttributeCtorInfos.end())
		{
			return it->second;
		}
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		auto stageStart = std::chrono::steady_clock::now();
#endif
		MethodRefInfo mri = {};
		ReadMethodRefInfoFromToken(nullptr, nullptr, DecodeTokenTableType(ctorMethodToken), DecodeTokenRowIndex(ctorMethodToken), mri);
		const MethodInfo* ctorMethod = GetMethodInfoFromMethodDef(mri.containerType, mri.methodDef);
		MethodIndex ctorIndex = il2cpp::vm::GlobalMetadata::GetMethodIndexFromDefinition(mri.methodDef);
		auto result = _customAttributeCtorInfos.emplace(ctorMethodToken, CustomAttributeCtorInfo{ ctorMethod, ctorIndex, mri.methodDef->parameterCount });
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		RecordMetadataInitStage(_index, "LazyCustomAttributeCtorResolve", (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stageStart).count());
#endif
		return result.first->second;
	}

	void InterpreterImage::BuildCustomAttributesData(CustomAttributesInfo& cai, const Il2CppCustomAttributeTypeRange& curTypeRange)
	{
		hybridclr::interpreter::ExecutingInterpImageScope scope(hybridclr::interpreter::InterpreterModule::GetCurrentThreadMachineState(), this->_il2cppImage);
		_il2cppFormatCustomDataBlob.Reset();
		const Il2CppCustomAttributeDataRange& nextTypeRange = *(&curTypeRange + 1);
		uint32_t attrCount = nextTypeRange.startOffset - curTypeRange.startOffset;
		IL2CPP_ASSERT(attrCount > 0 && attrCount < 1024);
		_il2cppFormatCustomDataBlob.WriteAttributeCount(attrCount);
		int32_t attrStartOffset = DecodeMetadataIndex(curTypeRange.startOffset);
		int32_t methodIndexDataOffset = _il2cppFormatCustomDataBlob.Size();
		_il2cppFormatCustomDataBlob.Skip(attrCount * sizeof(int32_t));
		for (uint32_t i = 0; i < attrCount; i++)
		{
			const CustomAttribute& ca = _customAttribues[attrStartOffset + (int32_t)i];
			const CustomAttributeCtorInfo& ctorInfo = GetOrCreateCustomAttributeCtorInfo(ca.ctorMethodToken);
			_il2cppFormatCustomDataBlob.WriteMethodIndex(methodIndexDataOffset, ctorInfo.methodIndex);
			methodIndexDataOffset += sizeof(int32_t);
			if (ca.value != 0)
			{
				BlobReader reader = _rawImage->GetBlobReaderByRawIndex(ca.value);
				ConvertILCustomAttributeData2Il2CppFormat(ctorInfo.method, reader);
			}
			else
			{
				IL2CPP_ASSERT(ctorInfo.parameterCount == 0);
				_il2cppFormatCustomDataBlob.WriteCompressedUint32(0);
				_il2cppFormatCustomDataBlob.WriteCompressedUint32(0);
				_il2cppFormatCustomDataBlob.WriteCompressedUint32(0);
			}
		}
		void* resultData = HYBRIDCLR_MALLOC(_il2cppFormatCustomDataBlob.Size());
		std::memcpy(resultData, _il2cppFormatCustomDataBlob.Data(), _il2cppFormatCustomDataBlob.Size());
		cai.dataStartPtr = resultData;
		cai.dataEndPtr = (uint8_t*)resultData + _il2cppFormatCustomDataBlob.Size();
	}

	void InterpreterImage::WriteEncodeTypeEnum(CustomAttributeDataWriter& writer, const Il2CppType* type)
	{
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
		if (type->type == IL2CPP_TYPE_ENUM || klass->enumtype)
		{
			writer.WriteByte((byte)IL2CPP_TYPE_ENUM);
			int32_t typeIndex = type->type == IL2CPP_TYPE_CLASS || type->type == IL2CPP_TYPE_VALUETYPE ? ((Il2CppTypeDefinition*)type->data.typeHandle)->byvalTypeIndex : AddIl2CppTypeCache(type);
			writer.WriteCompressedInt32(typeIndex);
		}
		else if (klass == il2cpp_defaults.systemtype_class)
		{
			writer.WriteByte((byte)IL2CPP_TYPE_IL2CPP_TYPE_INDEX);
		}
		else if (klass == il2cpp_defaults.string_class)
		{
			writer.WriteByte((byte)IL2CPP_TYPE_STRING);
		}
		else
		{
			writer.WriteByte((uint8_t)type->type);
		}
	}

	void InterpreterImage::ConvertBoxedValue(CustomAttributeDataWriter& writer, BlobReader& reader, bool writeType)
	{
		uint64_t obj = 0;
		Il2CppType kind = {};
		ReadCustomAttributeFieldOrPropType(reader, kind);
		ConvertFixedArg(writer, reader, &kind, true);
	}

	void InterpreterImage::ConvertSystemType(CustomAttributeDataWriter& writer, BlobReader& reader, bool writeType)
	{
		if (writeType)
		{
			writer.WriteByte((byte)IL2CPP_TYPE_IL2CPP_TYPE_INDEX);
		}
		Il2CppString* fullName = ReadSerString(reader);
		if (!fullName)
		{
			writer.WriteCompressedInt32(-1);
			return;
		}
		Il2CppReflectionType* type = ReadAttributeTypeName(fullName);
		if (!type)
		{
			std::string stdTypeName = il2cpp::utils::StringUtils::Utf16ToUtf8(fullName->chars);
			TEMP_FORMAT(errMsg, "CustomAttribute fixed arg type:System.Type fullName:'%s' not find", stdTypeName.c_str());
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeLoadException(errMsg));
		}
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type->type);
		if (!klass->generic_class && (Il2CppTypeDefinition*)klass->typeMetadataHandle)
		{
			writer.WriteCompressedInt32(((Il2CppTypeDefinition*)klass->typeMetadataHandle)->byvalTypeIndex);
		}
		else
		{
			writer.WriteCompressedInt32(AddIl2CppTypeCache(type->type));
		}
	}

	void InterpreterImage::ConvertFixedArg(CustomAttributeDataWriter& writer, BlobReader& reader, const Il2CppType* type, bool writeType)
	{
		switch (type->type)
		{
		case IL2CPP_TYPE_BOOLEAN:
		case IL2CPP_TYPE_I1:
		case IL2CPP_TYPE_U1:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.Write(reader, 1);
			break;
		}
		case IL2CPP_TYPE_CHAR:
		case IL2CPP_TYPE_I2:
		case IL2CPP_TYPE_U2:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.Write(reader, 2);
			break;
		}
		case IL2CPP_TYPE_I4:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.WriteCompressedInt32((int32_t)reader.Read32());
			break;
		}
		case IL2CPP_TYPE_U4:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.WriteCompressedUint32(reader.Read32());
			break;
		}
		case IL2CPP_TYPE_R4:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.Write(reader, 4);
			break;
		}
		case IL2CPP_TYPE_I8:
		case IL2CPP_TYPE_U8:
		case IL2CPP_TYPE_R8:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			writer.Write(reader, 8);
			break;
		}
		case IL2CPP_TYPE_SZARRAY:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			int32_t numElem = (int32_t)reader.Read32();
			writer.WriteCompressedInt32(numElem);
			if (numElem != -1)
			{
				//Il2CppType kind = {};
				//ReadCustomAttributeFieldOrPropType(reader, kind);
				const Il2CppType* eleType = type->data.type;
				WriteEncodeTypeEnum(writer, eleType);
				if (eleType->type == IL2CPP_TYPE_OBJECT)
				{
					// kArrayTypeWithDifferentElements
					writer.WriteByte(1);
					for (uint16_t i = 0; i < numElem; i++)
					{
						ConvertBoxedValue(writer, reader, false);
					}
				}
				else
				{
					// all element type is same.
					writer.WriteByte(0);
					for (uint16_t i = 0; i < numElem; i++)
					{
						ConvertFixedArg(writer, reader, eleType, false);
					}
				}

			}
			break;
		}
		case IL2CPP_TYPE_STRING:
		{
			if (writeType)
			{
				writer.WriteByte((uint8_t)type->type);
			}
			byte b = reader.PeekByte();
			if (b == 0xFF)
			{
				reader.SkipByte();
				writer.WriteCompressedInt32(-1);
			}
			else if (b == 0)
			{
				reader.SkipByte();
				writer.WriteCompressedInt32(0);
			}
			else
			{
				const byte* beginDataPtr = reader.GetDataOfReadPosition();
				uint32_t len = reader.ReadCompressedUint32();
				writer.WriteCompressedInt32((int32_t)len);
				writer.WriteBytes(reader.GetDataOfReadPosition(), len);
				reader.SkipBytes(len);
			}
			break;
		}
		case IL2CPP_TYPE_OBJECT:
		{
			ConvertBoxedValue(writer, reader, writeType);
			break;
		}
		case IL2CPP_TYPE_CLASS:
		{
			Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
			if (!klass)
			{
				RaiseExecutionEngineException("type not find");
			}
			if (klass->enumtype)
			{
				if (writeType)
				{
					writer.WriteByte((byte)IL2CPP_TYPE_ENUM);
					int32_t typeIndex = klass->generic_class ? AddIl2CppTypeCache(type) :
						((Il2CppTypeDefinition*)type->data.typeHandle)->byvalTypeIndex;
					writer.WriteCompressedInt32(typeIndex);
				}
				ConvertFixedArg(writer, reader, &klass->element_class->byval_arg, false);
			}
			else if (klass == il2cpp_defaults.string_class)
			{
				if (writeType)
				{
					writer.WriteByte((uint8_t)IL2CPP_TYPE_STRING);
				}
				byte b = reader.PeekByte();
				if (b == 0xFF)
				{
					reader.SkipByte();
					writer.WriteCompressedInt32(-1);
				}
				else if (b == 0)
				{
					reader.SkipByte();
					writer.WriteCompressedInt32(0);
				}
				else
				{
					uint32_t len = reader.ReadCompressedUint32();
					writer.WriteCompressedInt32((int32_t)len);
					writer.WriteBytes(reader.GetDataOfReadPosition(), len);
					reader.SkipBytes(len);
				}
			}
			else if (klass == il2cpp_defaults.object_class)
			{
				ConvertBoxedValue(writer, reader, writeType);
			}
			else if (klass == il2cpp_defaults.systemtype_class)
			{
				ConvertSystemType(writer, reader, writeType);
			}
			else
			{
				TEMP_FORMAT(errMsg, "fixed arg type:%s.%s not support", klass->namespaze, klass->name);
				RaiseNotSupportedException(errMsg);
			}
			break;
		}
		case IL2CPP_TYPE_VALUETYPE:
		{
			Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
			if (writeType)
			{
				writer.WriteByte((byte)IL2CPP_TYPE_ENUM);
				IL2CPP_ASSERT(klass->enumtype);
				int32_t typeIndex = klass->generic_class ? AddIl2CppTypeCache(type) : ((Il2CppTypeDefinition*)type->data.typeHandle)->byvalTypeIndex;
				writer.WriteCompressedInt32(typeIndex);
			}
			ConvertFixedArg(writer, reader, &klass->element_class->byval_arg, false);
			break;
		}
		case IL2CPP_TYPE_SYSTEM_TYPE:
		{
			ConvertSystemType(writer, reader, true);
			break;
		}
		case IL2CPP_TYPE_BOXED_OBJECT:
		{
			uint8_t fieldOrPropType = reader.ReadByte();
			IL2CPP_ASSERT(fieldOrPropType == 0x51);
			ConvertBoxedValue(writer, reader, writeType);
			break;
		}
		case IL2CPP_TYPE_ENUM:
		{
			Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type);
			IL2CPP_ASSERT(klass->enumtype);
			if (writeType)
			{
				int32_t typeIndex = klass->generic_class ? AddIl2CppTypeCache(type) : ((Il2CppTypeDefinition*)type->data.typeHandle)->byvalTypeIndex;
				writer.WriteCompressedInt32(typeIndex);
			}
			ConvertFixedArg(writer, reader, &klass->element_class->byval_arg, false);
			break;
		}
		default:
		{
			RaiseExecutionEngineException("not support fixed argument type");
		}
		}
	}

	void InterpreterImage::GetFieldDeclaringTypeIndexAndFieldIndexByName(const Il2CppTypeDefinition* declaringType, const char* name, int32_t& typeIndex, int32_t& fieldIndex)
	{
		Il2CppClass* klass = il2cpp::vm::GlobalMetadata::GetTypeInfoFromHandle((Il2CppMetadataTypeHandle)declaringType);
		FieldInfo* field = il2cpp::vm::Class::GetFieldFromName(klass, name);
		if (!field)
		{
			RaiseExecutionEngineException("GetFieldDeclaringTypeIndexAndFieldIndexByName can't find field");
		}
		if (field->parent == klass)
		{
			typeIndex = kTypeDefinitionIndexInvalid;
		}
		else
		{
			klass = field->parent;
			if (klass->generic_class)
			{
				RaiseExecutionEngineException("GetFieldDeclaringTypeIndexAndFieldIndexByName doesn't support field of generic CustomAttribute");
			}
			typeIndex = il2cpp::vm::GlobalMetadata::GetIndexForTypeDefinition(klass);
		}
		fieldIndex = (int32_t)(field - klass->fields);
	}

	void InterpreterImage::GetPropertyDeclaringTypeIndexAndPropertyIndexByName(const Il2CppTypeDefinition* declaringType, const char* name, int32_t& typeIndex, int32_t& fieldIndex)
	{
		Il2CppClass* klass = il2cpp::vm::GlobalMetadata::GetTypeInfoFromHandle((Il2CppMetadataTypeHandle)declaringType);
		const PropertyInfo* propertyInfo = il2cpp::vm::Class::GetPropertyFromName(klass, name);
		if (!propertyInfo)
		{
			RaiseExecutionEngineException("GetFieldDeclaringTypeIndexAndFieldIndexByName can't find field");
		}
		if (propertyInfo->parent == klass)
		{
			typeIndex = kTypeDefinitionIndexInvalid;
		}
		else
		{
			klass = propertyInfo->parent;
			if (klass->generic_class)
			{
				RaiseExecutionEngineException("GetPropertyDeclaringTypeIndexAndPropertyIndexByName doesn't support field of generic CustomAttribute");
			}
			typeIndex = il2cpp::vm::GlobalMetadata::GetIndexForTypeDefinition(klass);
		}
#if UNITY_ENGINE_TUANJIE
		fieldIndex = -1;
		for (int32_t i = 0; i < klass->property_count; i++)
		{
			if (klass->properties[i] == propertyInfo)
			{
				fieldIndex = i;
				break;;
			}
		}
		IL2CPP_ASSERT(fieldIndex != -1);
#else
		fieldIndex = (int32_t)(propertyInfo - klass->properties);
#endif
	}

	void InterpreterImage::ConvertILCustomAttributeData2Il2CppFormat(const MethodInfo* ctorMethod, BlobReader& reader)
	{
		uint16_t prolog = reader.Read16();
		IL2CPP_ASSERT(prolog == 0x0001);
		IL2CPP_ASSERT(!ctorMethod->is_generic);

		_tempCtorArgBlob.Reset();
		for (uint16_t i = 0; i < ctorMethod->parameters_count; i++)
		{
			const Il2CppType* paramType = GET_METHOD_PARAMETER_TYPE(ctorMethod->parameters[i]);
			ConvertFixedArg(_tempCtorArgBlob, reader, paramType, true);
		}

		uint16_t numNamed = reader.Read16();

		uint32_t fieldCount = 0;
		uint32_t propertyCount = 0;
		_tempFieldBlob.Reset();
		_tempPropertyBlob.Reset();
		const Il2CppTypeDefinition* declaringType = GetUnderlyingTypeDefinition(&ctorMethod->klass->byval_arg);
		for (uint16_t idx = 0; idx < numNamed; idx++)
		{
			byte fieldOrPropTypeTag = reader.ReadByte();
			IL2CPP_ASSERT(fieldOrPropTypeTag == 0x53 || fieldOrPropTypeTag == 0x54);
			Il2CppType fieldOrPropType = {};
			ReadCustomAttributeFieldOrPropType(reader, fieldOrPropType);
			Il2CppString* fieldOrPropName = ReadSerString(reader);
			std::string stdStrName = il2cpp::utils::StringUtils::Utf16ToUtf8(fieldOrPropName->chars);
			const char* cstrName = stdStrName.c_str();
			int32_t fieldOrPropertyDeclaringTypeIndex = kTypeIndexInvalid;
			int32_t fieldOrPropertyIndex = 0;
			if (fieldOrPropTypeTag == 0x53)
			{
				++fieldCount;
				ConvertFixedArg(_tempFieldBlob, reader, &fieldOrPropType, true);
				GetFieldDeclaringTypeIndexAndFieldIndexByName(declaringType, cstrName, fieldOrPropertyDeclaringTypeIndex, fieldOrPropertyIndex);
				if (fieldOrPropertyDeclaringTypeIndex == kTypeDefinitionIndexInvalid)
				{
					_tempFieldBlob.WriteCompressedInt32(fieldOrPropertyIndex);
				}
				else
				{
					_tempFieldBlob.WriteCompressedInt32(-fieldOrPropertyIndex - 1);
					_tempFieldBlob.WriteCompressedUint32(fieldOrPropertyDeclaringTypeIndex);
				}
			}
			else
			{
				++propertyCount;
				ConvertFixedArg(_tempPropertyBlob, reader, &fieldOrPropType, true);
				GetPropertyDeclaringTypeIndexAndPropertyIndexByName(declaringType, cstrName, fieldOrPropertyDeclaringTypeIndex, fieldOrPropertyIndex);
				if (fieldOrPropertyDeclaringTypeIndex == kTypeDefinitionIndexInvalid)
				{
					_tempPropertyBlob.WriteCompressedInt32(fieldOrPropertyIndex);
				}
				else
				{
					_tempPropertyBlob.WriteCompressedInt32(-fieldOrPropertyIndex - 1);
					_tempPropertyBlob.WriteCompressedUint32(fieldOrPropertyDeclaringTypeIndex);
				}
			}
		}
		_il2cppFormatCustomDataBlob.WriteCompressedUint32(ctorMethod->parameters_count);
		_il2cppFormatCustomDataBlob.WriteCompressedUint32(fieldCount);
		_il2cppFormatCustomDataBlob.WriteCompressedUint32(propertyCount);
		_il2cppFormatCustomDataBlob.Write(_tempCtorArgBlob);
		_il2cppFormatCustomDataBlob.Write(_tempFieldBlob);
		_il2cppFormatCustomDataBlob.Write(_tempPropertyBlob);
	}
#endif

#if !HYBRIDCLR_UNITY_2021_OR_NEW

	void InterpreterImage::ConstructCustomAttribute(BlobReader& reader, Il2CppObject* obj, const MethodInfo* ctorMethod)
	{
		uint16_t prolog = reader.Read16();
		IL2CPP_ASSERT(prolog == 0x0001);
		if (ctorMethod->parameters_count == 0)
		{
			il2cpp::vm::Runtime::Invoke(ctorMethod, obj, nullptr, nullptr);
		}
		else
		{
			int32_t argSize = sizeof(uint64_t) * ctorMethod->parameters_count;
			uint64_t* argDatas = (uint64_t*)alloca(argSize);
			std::memset(argDatas, 0, argSize);
			void** argPtrs = (void**)alloca(sizeof(void*) * ctorMethod->parameters_count); // same with argDatas
			for (uint8_t i = 0; i < ctorMethod->parameters_count; i++)
			{
				argPtrs[i] = argDatas + i;
				const Il2CppType* paramType = GET_METHOD_PARAMETER_TYPE(ctorMethod->parameters[i]);
				ReadFixedArg(reader, paramType, argDatas + i);
				Il2CppClass* paramKlass = il2cpp::vm::Class::FromIl2CppType(paramType);
				if (!IS_CLASS_VALUE_TYPE(paramKlass))
				{
					argPtrs[i] = (void*)argDatas[i];
				}
			}
			il2cpp::vm::Runtime::Invoke(ctorMethod, obj, argPtrs, nullptr);
			// clear ref. may not need. gc memory barrier
			std::memset(argDatas, 0, argSize);
		}
		uint16_t numNamed = reader.Read16();
		Il2CppClass* klass = obj->klass;
		for (uint16_t idx = 0; idx < numNamed; idx++)
		{
			byte fieldOrPropTypeTag = reader.ReadByte();
			IL2CPP_ASSERT(fieldOrPropTypeTag == 0x53 || fieldOrPropTypeTag == 0x54);
			Il2CppType fieldOrPropType = {};
			ReadCustomAttributeFieldOrPropType(reader, fieldOrPropType);
			Il2CppString* fieldOrPropName = ReadSerString(reader);
			std::string stdStrName = il2cpp::utils::StringUtils::Utf16ToUtf8(fieldOrPropName->chars);
			const char* cstrName = stdStrName.c_str();
			uint64_t value = 0;
			ReadFixedArg(reader, &fieldOrPropType, &value);
			if (fieldOrPropTypeTag == 0x53)
			{
				FieldInfo* field = il2cpp::vm::Class::GetFieldFromName(klass, cstrName);
				if (!field)
				{
					TEMP_FORMAT(errMsg, "CustomAttribute field missing. klass:%s.%s field:%s", klass->namespaze, klass->name, cstrName);
					il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeInitializationException(errMsg, nullptr));
				}
				IL2CPP_ASSERT(IsTypeEqual(&fieldOrPropType, field->type));
				il2cpp::vm::Field::SetValue(obj, field, &value);
			}
			else
			{
				const PropertyInfo* prop = il2cpp::vm::Class::GetPropertyFromName(klass, cstrName);
				if (!prop)
				{
					TEMP_FORMAT(errMsg, "CustomAttribute property missing. klass:%s property:%s", klass->name, cstrName);
					il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeInitializationException(errMsg, nullptr));
				}
				IL2CPP_ASSERT(IsTypeEqual(&fieldOrPropType, GET_METHOD_PARAMETER_TYPE(prop->set->parameters[0])));
				Il2CppException* ex = nullptr;
				Il2CppClass* propKlass = il2cpp::vm::Class::FromIl2CppType(&fieldOrPropType);
				IL2CPP_ASSERT(propKlass);
				void* args[] = { (IS_CLASS_VALUE_TYPE(propKlass) ? &value : (void*)value) };
				il2cpp::vm::Runtime::Invoke(prop->set, obj, args, &ex);
				if (ex)
				{
					il2cpp::vm::Exception::Raise(ex);
				}
			}
		}
	}

	CustomAttributesCache* InterpreterImage::GenerateCustomAttributesCacheInternal(CustomAttributeIndex index)
	{
		IL2CPP_ASSERT(index != kCustomAttributeIndexInvalid);
		IL2CPP_ASSERT(index < (CustomAttributeIndex)_customAttributeHandles.size());
		CustomAttributesCache* cache = il2cpp::os::Atomic::LoadPointerAcquire(&_customAttribtesCaches[index]);
		if (cache)
		{
			return cache;
		}

		Il2CppCustomAttributeTypeRange& typeRange = _customAttributeHandles[index];

		hybridclr::interpreter::ExecutingInterpImageScope scope(hybridclr::interpreter::InterpreterModule::GetCurrentThreadMachineState(), this->_il2cppImage);

		cache = (CustomAttributesCache*)IL2CPP_CALLOC(1, sizeof(CustomAttributesCache));
		int32_t count= (int32_t)typeRange.count;
		cache->count = count;
		cache->attributes = (Il2CppObject**)il2cpp::gc::GarbageCollector::AllocateFixed(sizeof(Il2CppObject*) * count, 0);

		int32_t start = DecodeMetadataIndex(GET_CUSTOM_ATTRIBUTE_TYPE_RANGE_START(typeRange));
		for (int32_t i = 0; i < count; i++)
		{
			int32_t attrIndex = start + i;
			IL2CPP_ASSERT(attrIndex >= 0 && attrIndex < (int32_t)_customAttribues.size());
			CustomAttribute& ca = _customAttribues[attrIndex];
			MethodRefInfo mri = {};
			ReadMethodRefInfoFromToken(nullptr, nullptr, DecodeTokenTableType(ca.ctorMethodToken), DecodeTokenRowIndex(ca.ctorMethodToken), mri);
			const MethodInfo* ctorMethod = GetMethodInfoFromMethodDef(mri.containerType, mri.methodDef);
			IL2CPP_ASSERT(ctorMethod);
			Il2CppClass* klass = ctorMethod->klass;
			Il2CppObject* attr = il2cpp::vm::Object::New(klass);
			Il2CppArray* paramArr = nullptr;
			if (ca.value != 0)
			{
				BlobReader reader = _rawImage->GetBlobReaderByRawIndex(ca.value);
				ConstructCustomAttribute(reader, attr, ctorMethod);
			}
			else
			{
				IL2CPP_ASSERT(ctorMethod->parameters_count == 0);
				il2cpp::vm::Runtime::Invoke(ctorMethod, attr, nullptr, nullptr);
			}

			cache->attributes[i] = attr;
			HYBRIDCLR_SET_WRITE_BARRIER((void**)cache->attributes + i);
		}

		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		CustomAttributesCache* original = il2cpp::os::Atomic::LoadPointerAcquire(&_customAttribtesCaches[index]);
		if (original)
		{
			// A non-NULL return value indicates some other thread already generated this cache.
			// We need to cleanup the resources we allocated
			il2cpp::gc::GarbageCollector::FreeFixed(cache->attributes);
			HYBRIDCLR_FREE(cache);
			return original;
		}
		il2cpp::os::Atomic::PublishPointer(&_customAttribtesCaches[index], cache);
		return cache;
	}
#elif HYBRIDCLR_UNITY_2021
	CustomAttributesCache* InterpreterImage::GenerateCustomAttributesCacheInternal(CustomAttributeIndex index)
	{
		IL2CPP_ASSERT(index != kCustomAttributeIndexInvalid);
		IL2CPP_ASSERT(index < (CustomAttributeIndex)_customAttributeHandles.size());
		CustomAttributesCache* cache = il2cpp::os::Atomic::LoadPointerAcquire(&_customAttribtesCaches[index]);
		if (cache)
		{
			return cache;
		}

		hybridclr::interpreter::ExecutingInterpImageScope scope(hybridclr::interpreter::InterpreterModule::GetCurrentThreadMachineState(), this->_il2cppImage);

		Il2CppCustomAttributeTypeRange& typeRange = _customAttributeHandles[index];
		void* start;
		void* end;
		std::tie(start, end) = CreateCustomAttributeDataTuple(&typeRange);
		IL2CPP_ASSERT(start && end);

		il2cpp::metadata::CustomAttributeDataReader reader(start, end);

		cache = (CustomAttributesCache*)IL2CPP_CALLOC(1, sizeof(CustomAttributesCache));
		cache->count = (int)reader.GetCount();
		cache->attributes = (Il2CppObject**)il2cpp::gc::GarbageCollector::AllocateFixed(sizeof(Il2CppObject*) * cache->count, 0);

		il2cpp::metadata::CustomAttributeDataIterator iter = reader.GetDataIterator();
		for (int i = 0; i < cache->count; i++)
		{
			Il2CppException* exc = NULL;
			il2cpp::metadata::CustomAttributeCreator creator;
			if (reader.VisitCustomAttributeData(_il2cppImage, &iter, &creator, &exc))
			{
				cache->attributes[i] = creator.GetAttribute(&exc);
				HYBRIDCLR_SET_WRITE_BARRIER((void**)&cache->attributes[i]);
			}

			if (exc != NULL)
			{
				il2cpp::gc::GarbageCollector::FreeFixed(cache->attributes);
				HYBRIDCLR_FREE(cache);
				il2cpp::vm::Exception::Raise(exc);
			}
		}


		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		CustomAttributesCache* original = il2cpp::os::Atomic::LoadPointerAcquire(&_customAttribtesCaches[index]);
		if (original)
		{
			// A non-NULL return value indicates some other thread already generated this cache.
			// We need to cleanup the resources we allocated
			il2cpp::gc::GarbageCollector::FreeFixed(cache->attributes);
			HYBRIDCLR_FREE(cache);
			return original;
		}
		il2cpp::os::Atomic::PublishPointer(&_customAttribtesCaches[index], cache);
		return cache;
	}
#endif

	void InterpreterImage::InitModuleRefs()
	{
		const Table& moduleRefTb = _rawImage->GetTable(TableType::MODULEREF);
		_moduleRefs.reserve(moduleRefTb.rowNum);
		for (uint32_t rid = 1; rid <= moduleRefTb.rowNum; rid++)
		{
			TbModuleRef moduleRef = _rawImage->ReadModuleRef(rid);
			const char* moduleName = _rawImage->GetStringFromRawIndex(moduleRef.name);
			_moduleRefs.push_back(moduleName);
		}
	}

	void InterpreterImage::InitImplMaps()
	{
		const Table& implMapTb = _rawImage->GetTable(TableType::IMPLMAP);
		_implMapInfos.reserve(implMapTb.rowNum);
		for (uint32_t rid = 1; rid <= implMapTb.rowNum; rid++)
		{
			TbImplMap implMap = _rawImage->ReadImplMap(rid);
			if (implMap.importScope == 0 || implMap.importScope > _moduleRefs.size())
			{
				RaiseBadImageException("implementation map import scope is out of range");
			}
			TableType forwardedTable = (implMap.memberForwarded & 0x1) ? TableType::METHOD : TableType::FIELD;
			uint32_t forwardedRow = implMap.memberForwarded >> 1;
			if (forwardedRow == 0 || forwardedRow > _rawImage->GetTable(forwardedTable).rowNum)
			{
				RaiseBadImageException("implementation map forwarded member is out of range");
			}
			ImplMapInfo info = {};
			info.moduleName = _moduleRefs[implMap.importScope - 1];
			info.importName = _rawImage->GetStringFromRawIndex(implMap.importName);
			info.mappingFlags = implMap.mappingFlags;
			uint32_t memberForwardedToken = hybridclr::metadata::ConvertMemberForwardedToken2Token(implMap.memberForwarded);
			if (!_implMapInfos.emplace(memberForwardedToken, info).second)
			{
				RaiseBadImageException("member has multiple implementation map rows");
			}
		}
	}

	void InterpreterImage::InitMethodDefs0()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		const Table& methodTb = _rawImage->GetTable(TableType::METHOD);

		_methodDefines.resize(methodTb.rowNum);
		_methodMetadataDetails.reset(methodTb.rowNum > 0 ? new MethodMetadataDetail[methodTb.rowNum] : nullptr);
		for (Il2CppMethodDefinition& md : _methodDefines)
		{
			md.genericContainerIndex = kGenericContainerIndexInvalid;
		}
	}

	void InterpreterImage::InitMethodDefs()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		const Table& methodTb = _rawImage->GetTable(TableType::METHOD);

		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& typeDef = _typesDefines[i];
			uint32_t rawMethodStart = DecodeMetadataIndex(typeDef.methodStart);

			for (int m = 0; m < typeDef.method_count; m++)
			{
				Il2CppMethodDefinition& md = _methodDefines[rawMethodStart + m];
				md.declaringType = EncodeWithIndex(i);
			}
		}

		uint32_t paramTableRowNum = _rawImage->GetTable(TableType::PARAM).rowNum;
		if (methodTb.rowNum == 0 && paramTableRowNum != 0)
		{
			RaiseBadImageException("parameter rows exist without a declaring method");
		}
		uint32_t parameterStart = 0;
		for (uint32_t index = 0; index < methodTb.rowNum; index++)
		{
			Il2CppMethodDefinition& md = _methodDefines[index];
			MethodMetadataDetail& detail = _methodMetadataDetails[index];
			uint32_t rowIndex = index + 1;
			TbMethod methodData = _rawImage->ReadMethod(rowIndex);
			if (methodData.paramList == 0 || methodData.paramList - 1 > paramTableRowNum)
			{
				RaiseBadImageException("method parameter list index is out of range");
			}
			detail.signatureBlobIndex = methodData.signature;
			detail.rawParameterStart = methodData.paramList - 1;
			if (index == 0 && detail.rawParameterStart != 0)
			{
				RaiseBadImageException("method parameter ownership does not start at the first row");
			}
			const byte* signatureBlob = _rawImage->GetBlobFromRawIndex(methodData.signature);
			uint32_t valueLength;
			BlobReader::ReadCompressedUint32(signatureBlob, valueLength);
			const byte* signatureData = signatureBlob + valueLength;
			uint8_t signatureFlags = *signatureData++;
			if (signatureFlags & (uint8_t)MethodSigFlags::GENERIC)
			{
				BlobReader::ReadCompressedUint32(signatureData, valueLength);
				signatureData += valueLength;
			}
			uint32_t parameterCount = BlobReader::ReadCompressedUint32(signatureData, valueLength);
			detail.initialized = 0;
			if (parameterCount >= 256)
			{
				const Il2CppTypeDefinition& typeDef = _typesDefines[DecodeMetadataIndex(md.declaringType)];
				TEMP_FORMAT(errMsg, "method:%s.%s parameter count:%d is too large", _rawImage->GetStringFromRawIndex(DecodeMetadataIndex(typeDef.nameIndex)), _rawImage->GetStringFromRawIndex(methodData.name), parameterCount);
				RaiseExecutionEngineException(errMsg);
			}
			if (parameterStart > std::numeric_limits<uint32_t>::max() - parameterCount)
			{
				RaiseBadImageException("method parameter table is too large");
			}
			md.parameterStart = parameterStart;
			md.parameterCount = (uint16_t)parameterCount;
			parameterStart += parameterCount;

			md.nameIndex = EncodeWithIndex(methodData.name);
			//md.genericContainerIndex = kGenericContainerIndexInvalid;
			md.token = EncodeToken(TableType::METHOD, rowIndex);
			md.flags = methodData.flags;
			md.iflags = methodData.implFlags;
			md.slot = kInvalidIl2CppMethodSlot;
			if (index > 0)
			{
				MethodMetadataDetail& last = _methodMetadataDetails[index - 1];
				if (detail.rawParameterStart < last.rawParameterStart)
				{
					RaiseBadImageException("method parameter list is not monotonic");
				}
				last.rawParameterCount = detail.rawParameterStart - last.rawParameterStart;
			}
			if (index == methodTb.rowNum - 1)
			{
				detail.rawParameterCount = paramTableRowNum - detail.rawParameterStart;
			}
			//MethodBody& body = _methodBodies[index];
			//ReadMethodBody(md, methodData, body);
		}
		_paramCount = parameterStart;
		_params.Reset(parameterStart);

		for (uint32_t i = 0, n = typeDefTb.rowNum; i < n; i++)
		{
			Il2CppTypeDefinition& typeDef = _typesDefines[i];
			TypeDefinitionDetail& typeDetail = _typeDetails[i];
			uint32_t rawMethodStart = DecodeMetadataIndex(typeDef.methodStart);
			bool isInterface = IsInterface(typeDef.flags);
			uint16_t slotIdx = 0;
			typeDetail.virtualMethodCount = 0;
			for (int m = 0; m < typeDef.method_count; m++)
			{
				Il2CppMethodDefinition& md = _methodDefines[rawMethodStart + m];
				if (IsVirtualMethod(md.flags))
				{
					++typeDetail.virtualMethodCount;
				}
				const char* methodName = _rawImage->GetStringFromRawIndex(DecodeMetadataIndex(md.nameIndex));
				if (!std::strcmp(methodName, ".cctor"))
				{
					typeDef.bitfield |= (1 << (il2cpp::vm::kBitHasStaticConstructor - 1));
				}
				if (!std::strcmp(methodName, "Finalize"))
				{
					typeDef.bitfield |= (1 << (il2cpp::vm::kBitHasFinalizer - 1));
				}
				if (isInterface && IsInstanceMethod(&md) && IsVirtualMethod(md.flags))
				{
					md.slot = slotIdx++;
				}
			}
		}
	}

	void InterpreterImage::EnsureMethodMetadataInitialized(uint32_t index)
	{
		IL2CPP_ASSERT(index < _methodDefines.size());
		if (IsMetadataPublished(&_methodMetadataDetails[index].initialized))
		{
			return;
		}
		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		EnsureMethodMetadataInitializedLocked(index);
	}

	void InterpreterImage::EnsureMethodMetadataInitializedLocked(uint32_t index)
	{
		IL2CPP_ASSERT(index < _methodDefines.size());
		if (IsMetadataPublished(&_methodMetadataDetails[index].initialized))
		{
			return;
		}
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		auto stageStart = std::chrono::steady_clock::now();
#endif
		BuildMethodMetadata(index);
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		RecordMetadataInitStage(_index, "LazyMethodMetadata", (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stageStart).count());
#endif
		PublishMetadata(&_methodMetadataDetails[index].initialized);
		++_methodMetadataInitializedCount;
		IL2CPP_ASSERT(_methodMetadataInitializedCount <= _methodDefines.size());
		if (_methodMetadataInitializedCount == _methodDefines.size())
		{
			FreezeIl2CppTypeCache();
			std::unordered_map<uint32_t, uint32_t> emptyDefaultValueIndexes;
			_rawParamDefaultValueIndexes.swap(emptyDefaultValueIndexes);
		}
	}

	void InterpreterImage::EnsureTypeMethodMetadataInitializedLocked(const Il2CppTypeDefinition* typeDef)
	{
		IL2CPP_ASSERT(typeDef >= _typesDefines.data() && typeDef < _typesDefines.data() + _typesDefines.size());
		uint32_t methodStart = DecodeMetadataIndex(typeDef->methodStart);
		IL2CPP_ASSERT(methodStart <= _methodDefines.size());
		IL2CPP_ASSERT(typeDef->method_count <= _methodDefines.size() - methodStart);
		for (uint32_t index = methodStart, end = methodStart + typeDef->method_count; index < end; ++index)
		{
			EnsureMethodMetadataInitializedLocked(index);
		}
	}

	void InterpreterImage::EnsureFieldMetadataInitialized(uint32_t index)
	{
		IL2CPP_ASSERT(index < _fieldDetails.size());
		if (IsFieldMetadataInitialized(_fieldDetails[index]))
		{
			return;
		}
		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		EnsureFieldMetadataInitializedLocked(index);
	}

	void InterpreterImage::EnsureFieldMetadataInitializedLocked(uint32_t index)
	{
		IL2CPP_ASSERT(index < _fieldDetails.size());
		if (IsFieldMetadataInitialized(_fieldDetails[index]))
		{
			return;
		}
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		auto stageStart = std::chrono::steady_clock::now();
#endif
		BuildFieldMetadata(index);
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		RecordMetadataInitStage(_index, "LazyFieldMetadata", (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stageStart).count());
#endif
	}

	void InterpreterImage::EnsureTypeFieldMetadataInitializedLocked(const Il2CppTypeDefinition* typeDef)
	{
		IL2CPP_ASSERT(typeDef >= _typesDefines.data() && typeDef < _typesDefines.data() + _typesDefines.size());
		uint32_t fieldStart = DecodeMetadataIndex(typeDef->fieldStart);
		IL2CPP_ASSERT(fieldStart <= _fieldDetails.size());
		IL2CPP_ASSERT(typeDef->field_count <= _fieldDetails.size() - fieldStart);
		for (uint32_t index = fieldStart, end = fieldStart + typeDef->field_count; index < end; ++index)
		{
			EnsureFieldMetadataInitializedLocked(index);
		}
	}

	void InterpreterImage::BuildFieldMetadata(uint32_t index)
	{
		FieldDetail& field = _fieldDetails[index];
		TbField rawField = _rawImage->ReadField(index + 1);
		BlobReader fieldSigReader = _rawImage->GetBlobReaderByRawIndex(rawField.signature);
		FieldRefSig fieldSig;
		ReadFieldRefSig(fieldSigReader, GetGenericContainerByTypeDefRawIndex(DecodeMetadataIndex(field.typeDefIndex)), fieldSig);
		const Il2CppType* fieldType = fieldSig.type;
		// Field flags are only needed while materializing the type. Keep them out of
		// the load-time cache and read the compact raw row on first access.
		if (rawField.flags != 0)
		{
			Il2CppType typeWithAttrs = *fieldType;
			typeWithAttrs.attrs = rawField.flags;
			fieldType = MetadataPool::GetPooledIl2CppType(typeWithAttrs);
		}
		PublishFieldMetadata(field, AddIl2CppTypeCache(fieldType));
	}

	void InterpreterImage::BuildMethodMetadata(uint32_t index)
	{
		Il2CppMethodDefinition& md = _methodDefines[index];
		MethodMetadataDetail& detail = _methodMetadataDetails[index];
		Il2CppTypeDefinition& typeDef = _typesDefines[DecodeMetadataIndex(md.declaringType)];
		BlobReader methodSigReader = _rawImage->GetBlobReaderByRawIndex(detail.signatureBlobIndex);
		uint32_t rawParameterStart = detail.rawParameterStart;
		uint32_t rawParameterCount = detail.rawParameterCount;
		uint32_t paramTableRowNum = _rawImage->GetTable(TableType::PARAM).rowNum;
		if (rawParameterStart > paramTableRowNum || rawParameterCount > paramTableRowNum - rawParameterStart)
		{
			RaiseBadImageException("method parameter range is out of bounds");
		}

		uint32_t actualParamStart = md.parameterStart;
		ReadMethodDefSig(
			methodSigReader,
			GetGenericContainerByTypeDefinition(&typeDef),
			GetGenericContainerByRawIndex(DecodeMetadataIndex(md.genericContainerIndex)),
			md,
			actualParamStart,
			md.parameterCount);
		uint32_t actualParamCount = md.parameterCount;
		md.parameterStart = actualParamStart;
		md.parameterCount = (uint16_t)actualParamCount;
		uint64_t seenSequences[4] = {};
		for (uint32_t paramIndex = 0; paramIndex < rawParameterCount; ++paramIndex)
		{
			uint32_t paramRowIndex = rawParameterStart + paramIndex + 1;
			TbParam data = _rawImage->ReadParam(paramRowIndex);
			if (data.sequence > md.parameterCount)
			{
				RaiseBadImageException("method parameter sequence exceeds signature parameter count");
			}
			uint64_t sequenceMask = (uint64_t)1 << (data.sequence & 63);
			uint64_t& seenSequenceWord = seenSequences[data.sequence >> 6];
			if ((seenSequenceWord & sequenceMask) != 0)
			{
				RaiseBadImageException("method parameter sequence is duplicated");
			}
			seenSequenceWord |= sequenceMask;
			if (data.sequence > 0)
			{
				uint32_t actualParamIndex = actualParamStart + data.sequence - 1;
				ParamDetail& paramDetail = _params[actualParamIndex];
				Il2CppParameterDefinition& pd = paramDetail.paramDef;
				IL2CPP_ASSERT(paramDetail.parameterIndex == data.sequence - 1);
				pd.nameIndex = EncodeWithIndex(data.name);
				pd.token = EncodeToken(TableType::PARAM, paramRowIndex);
				if (data.flags)
				{
					const Il2CppType* paramType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(pd.typeIndex);
					Il2CppType* newType = MetadataPool::ShallowCloneIl2CppType(paramType);
					newType->attrs = data.flags;
					pd.typeIndex = AddIl2CppTypeCache(newType);
				}
				if (data.flags & PARAM_ATTRIBUTE_HAS_DEFAULT)
				{
					auto defaultValue = _rawParamDefaultValueIndexes.find(paramRowIndex - 1);
					if (defaultValue != _rawParamDefaultValueIndexes.end())
					{
						paramDetail.defaultValueIndex = defaultValue->second;
					}
				}
			}
#if SUPPORT_METHOD_RETURN_TYPE_CUSTOM_ATTRIBUTE
			else
			{
				md.returnParameterToken = EncodeToken(TableType::PARAM, paramRowIndex);
			}
#endif
		}
	}

	const il2cpp::utils::dynamic_array<MethodImpl> InterpreterImage::GetTypeMethodImplByTypeDefinition(const Il2CppTypeDefinition* typeDef)
	{
		uint32_t index = (uint32_t)(typeDef - &_typesDefines[0]);
		IL2CPP_ASSERT(index < (uint32_t)_typeDetails.size());
		TypeDefinitionDetail& tdd = _typeDetails[index];
		il2cpp::utils::dynamic_array<MethodImpl> methodImpls(tdd.methodImplCount);

		for (uint32_t i = 0; i < tdd.methodImplCount; i++)
		{
			uint32_t index = tdd.methodImplStart + i;
			TbMethodImpl data = _rawImage->ReadMethodImpl(index + 1);
			Il2CppTypeDefinition& typeDef = _typesDefines[data.classIdx - 1];
			Il2CppGenericContainer* gc = GetGenericContainerByTypeDefinition(&typeDef);
			MethodImpl& impl = methodImpls[i];
			ReadMethodRefInfoFromToken(gc, nullptr, DecodeMethodDefOrRefCodedIndexTableType(data.methodBody), DecodeMethodDefOrRefCodedIndexRowIndex(data.methodBody), impl.body);
			ReadMethodRefInfoFromToken(gc, nullptr, DecodeMethodDefOrRefCodedIndexTableType(data.methodDeclaration), DecodeMethodDefOrRefCodedIndexRowIndex(data.methodDeclaration), impl.declaration);
		}
		return methodImpls;
	}

	void InterpreterImage::InitMethodImpls0()
	{
		const Table& miTb = _rawImage->GetTable(TableType::METHODIMPL);
		uint32_t lastType = 0;
		for (uint32_t i = 0; i < miTb.rowNum; i++)
		{
			TbMethodImpl data = _rawImage->ReadMethodImpl(i + 1);
			if (data.classIdx == 0 || data.classIdx > _typesDefines.size())
			{
				RaiseBadImageException("method implementation class index is out of range");
			}
			ValidateMethodDefOrRefCodedIndex(*_rawImage, data.methodBody,
				"method implementation body coded index is invalid");
			ValidateMethodDefOrRefCodedIndex(*_rawImage, data.methodDeclaration,
				"method implementation declaration coded index is invalid");
			uint32_t typeIndex = data.classIdx - 1;
			TypeDefinitionDetail& tdd = _typeDetails[typeIndex];
			if (tdd.methodImplCount == 0)
			{
				tdd.methodImplStart = i;
			}
			else if (lastType != data.classIdx)
			{
				RaiseBadImageException("method implementations for one type are not contiguous");
			}
			++tdd.methodImplCount;
			lastType = data.classIdx;
		}
	}

	void InterpreterImage::InitProperties()
	{
		const Table& propertyMapTb = _rawImage->GetTable(TableType::PROPERTYMAP);
		const Table& propertyTb = _rawImage->GetTable(TableType::PROPERTY);

		Il2CppTypeDefinition* last = nullptr;
		uint32_t lastParent = 0;
		uint32_t lastPropertyList = 0;
		for (uint32_t rowIndex = 1; rowIndex <= propertyMapTb.rowNum; rowIndex++)
		{
			TbPropertyMap data = _rawImage->ReadPropertyMap(rowIndex);
			if (data.parent == 0 || data.parent > _typesDefines.size())
			{
				RaiseBadImageException("property map parent index is out of range");
			}
			if (data.parent <= lastParent)
			{
				RaiseBadImageException("property map parents are duplicated or not sorted");
			}
			if (data.propertyList == 0 || data.propertyList - 1 > propertyTb.rowNum)
			{
				RaiseBadImageException("property map list index is out of range");
			}
			if (last != nullptr && data.propertyList < lastPropertyList)
			{
				RaiseBadImageException("property map list is not monotonic");
			}
			Il2CppTypeDefinition* typeDef = &_typesDefines[data.parent - 1];
			typeDef->propertyStart = EncodeWithIndex(data.propertyList); // start from 1
			if (last != nullptr)
			{
				uint32_t count = data.propertyList - lastPropertyList;
				if (count > std::numeric_limits<uint16_t>::max())
				{
					RaiseBadImageException("type property count exceeds runtime limits");
				}
				last->property_count = static_cast<uint16_t>(count);
			}
			last = typeDef;
			lastParent = data.parent;
			lastPropertyList = data.propertyList;
		}
		if (last)
		{
			uint32_t count = propertyTb.rowNum - (lastPropertyList - 1);
			if (count > std::numeric_limits<uint16_t>::max())
			{
				RaiseBadImageException("type property count exceeds runtime limits");
			}
			last->property_count = static_cast<uint16_t>(count);
		}
	}

	void InterpreterImage::BuildProperties()
	{
		const Table& propertyTb = _rawImage->GetTable(TableType::PROPERTY);
		_propeties.reserve(propertyTb.rowNum);
		for (uint32_t rowIndex = 1; rowIndex <= propertyTb.rowNum; rowIndex++)
		{
			TbProperty data = _rawImage->ReadProperty(rowIndex);
			_propeties.push_back({ _rawImage->GetStringFromRawIndex(data.name), data.flags, data.type, 0, 0
				, nullptr
				, { (StringIndex)EncodeWithIndex(data.name), kMethodIndexInvalid, kMethodIndexInvalid, (uint32_t)data.flags, EncodeToken(TableType::PROPERTY, rowIndex)}
				});
		}
#if HYBRIDCLR_UNITY_2019
		for (const Il2CppTypeDefinition& typeDef : _typesDefines)
		{
			if (typeDef.property_count == 0)
			{
				continue;
			}
			for (int32_t start = DecodeMetadataIndex(typeDef.propertyStart), i = 0; i < typeDef.property_count; i++)
			{
				_propeties[start + i - 1].declaringType = &typeDef;
			}
		}
#endif
	}

	void InterpreterImage::InitEvents()
	{
		const Table& eventMapTb = _rawImage->GetTable(TableType::EVENTMAP);
		const Table& eventTb = _rawImage->GetTable(TableType::EVENT);

		Il2CppTypeDefinition* last = nullptr;
		uint32_t lastParent = 0;
		uint32_t lastEventList = 0;
		for (uint32_t rowIndex = 1; rowIndex <= eventMapTb.rowNum; rowIndex++)
		{
			TbEventMap data = _rawImage->ReadEventMap(rowIndex);
			if (data.parent == 0 || data.parent > _typesDefines.size())
			{
				RaiseBadImageException("event map parent index is out of range");
			}
			if (data.parent <= lastParent)
			{
				RaiseBadImageException("event map parents are duplicated or not sorted");
			}
			if (data.eventList == 0 || data.eventList - 1 > eventTb.rowNum)
			{
				RaiseBadImageException("event map list index is out of range");
			}
			if (last != nullptr && data.eventList < lastEventList)
			{
				RaiseBadImageException("event map list is not monotonic");
			}
			Il2CppTypeDefinition* typeDef = &_typesDefines[data.parent - 1];
			typeDef->eventStart = EncodeWithIndex(data.eventList); // start from 1
			if (last != nullptr)
			{
				uint32_t count = data.eventList - lastEventList;
				if (count > std::numeric_limits<uint16_t>::max())
				{
					RaiseBadImageException("type event count exceeds runtime limits");
				}
				last->event_count = static_cast<uint16_t>(count);
			}
			last = typeDef;
			lastParent = data.parent;
			lastEventList = data.eventList;
		}
		if (last)
		{
			uint32_t count = eventTb.rowNum - (lastEventList - 1);
			if (count > std::numeric_limits<uint16_t>::max())
			{
				RaiseBadImageException("type event count exceeds runtime limits");
			}
			last->event_count = static_cast<uint16_t>(count);
		}
	}

	void InterpreterImage::BuildEvents()
	{
		const Table& eventTb = _rawImage->GetTable(TableType::EVENT);
		_events.reserve(eventTb.rowNum);
		for (uint32_t rowIndex = 1; rowIndex <= eventTb.rowNum; rowIndex++)
		{
			TbEvent data = _rawImage->ReadEvent(rowIndex);
			_events.push_back({ _rawImage->GetStringFromRawIndex(data.name), data.eventFlags, data.eventType, 0, 0, 0
#if HYBRIDCLR_UNITY_2019
				, nullptr
				, { (StringIndex)EncodeWithIndex(data.name), kTypeIndexInvalid, kMethodIndexInvalid, kMethodIndexInvalid, kMethodIndexInvalid, EncodeToken(TableType::EVENT, rowIndex)}
#endif
				});
		}
#if HYBRIDCLR_UNITY_2019
		for (const Il2CppTypeDefinition& typeDef : _typesDefines)
		{
			if (typeDef.event_count == 0)
			{
				continue;
			}
			for (int32_t start = DecodeMetadataIndex(typeDef.eventStart), i = 0; i < typeDef.event_count; i++)
			{
				EventDetail& ed = _events[start + i - 1];
				ed.declaringType = &typeDef;
				ed.il2cppDefinition.typeIndex = typeDef.byvalTypeIndex;
			}
		}
#endif
	}

	void InterpreterImage::EnsurePropertyEventMetadataInitialized()
	{
		if (IsMetadataPublished(&_propertyEventMetadataInitialized))
		{
			return;
		}
		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		if (IsMetadataPublished(&_propertyEventMetadataInitialized))
		{
			return;
		}
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		auto stageStart = std::chrono::steady_clock::now();
#endif
		BuildProperties();
		BuildEvents();
		InitMethodSemantics();
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		RecordMetadataInitStage(_index, "LazyPropertyEventMetadata", (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stageStart).count());
#endif
		PublishMetadata(&_propertyEventMetadataInitialized);
	}


	void InterpreterImage::InitMethodSemantics()
	{
		const Table& msTb = _rawImage->GetTable(TableType::METHODSEMANTICS);
		const uint16_t propertySemantics =
			(uint16_t)MethodSemanticsAttributes::Setter |
			(uint16_t)MethodSemanticsAttributes::Getter |
			(uint16_t)MethodSemanticsAttributes::Other;
		const uint16_t eventSemantics =
			(uint16_t)MethodSemanticsAttributes::AddOn |
			(uint16_t)MethodSemanticsAttributes::RemoveOn |
			(uint16_t)MethodSemanticsAttributes::Fire |
			(uint16_t)MethodSemanticsAttributes::Other;
		for (uint32_t rowIndex = 1; rowIndex <= msTb.rowNum; rowIndex++)
		{
			TbMethodSemantics data = _rawImage->ReadMethodSemantics(rowIndex);
			uint32_t method = data.method;
			uint16_t semantics = data.semantics;
			if (method == 0 || method > _methodDefines.size())
			{
				RaiseBadImageException("method semantics method index is out of range");
			}
			TableType tableType = DecodeHasSemanticsCodedIndexTableType(data.association);
			uint32_t associationRowIndex = DecodeHasSemanticsCodedIndexRowIndex(data.association);
			if (semantics == 0 ||
				(tableType == TableType::PROPERTY && (semantics & ~propertySemantics) != 0) ||
				(tableType == TableType::EVENT && (semantics & ~eventSemantics) != 0))
			{
				RaiseBadImageException("method semantics flags do not match the association type");
			}

			const Il2CppMethodDefinition& methodDef = _methodDefines[method - 1];
			uint32_t declaringTypeIndex = DecodeMetadataIndex(methodDef.declaringType);
			if (declaringTypeIndex >= _typesDefines.size())
			{
				RaiseBadImageException("method semantics method has no valid declaring type");
			}
			const Il2CppTypeDefinition& declaringType = _typesDefines[declaringTypeIndex];
			if (tableType == TableType::PROPERTY)
			{
				if (associationRowIndex == 0 || associationRowIndex > _propeties.size())
				{
					RaiseBadImageException("method semantics property index is out of range");
				}
				uint32_t propertyStart = DecodeMetadataIndex(declaringType.propertyStart);
				if (declaringType.property_count == 0 || associationRowIndex < propertyStart ||
					associationRowIndex - propertyStart >= declaringType.property_count)
				{
					RaiseBadImageException("property accessor method belongs to a different type");
				}
			}
			else
			{
				if (associationRowIndex == 0 || associationRowIndex > _events.size())
				{
					RaiseBadImageException("method semantics event index is out of range");
				}
				uint32_t eventStart = DecodeMetadataIndex(declaringType.eventStart);
				if (declaringType.event_count == 0 || associationRowIndex < eventStart ||
					associationRowIndex - eventStart >= declaringType.event_count)
				{
					RaiseBadImageException("event accessor method belongs to a different type");
				}
			}

			uint32_t propertyOrEventIndex = associationRowIndex - 1;
			if (semantics & (uint16_t)MethodSemanticsAttributes::Getter)
			{
				PropertyDetail& pd = _propeties[propertyOrEventIndex];
				if (pd.getterMethodIndex != 0)
				{
					RaiseBadImageException("property has multiple getter methods");
				}
				pd.getterMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				pd.il2cppDefinition.get = method - DecodeMetadataIndex(pd.declaringType->methodStart) - 1;
#endif
			}
			if (semantics & (uint16_t)MethodSemanticsAttributes::Setter)
			{
				PropertyDetail& pd = _propeties[propertyOrEventIndex];
				if (pd.setterMethodIndex != 0)
				{
					RaiseBadImageException("property has multiple setter methods");
				}
				pd.setterMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				pd.il2cppDefinition.set = method - DecodeMetadataIndex(pd.declaringType->methodStart) - 1;
#endif
			}
			if (semantics & (uint16_t)MethodSemanticsAttributes::AddOn)
			{
				EventDetail& ed = _events[propertyOrEventIndex];
				if (ed.addMethodIndex != 0)
				{
					RaiseBadImageException("event has multiple add methods");
				}
				ed.addMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				ed.il2cppDefinition.add = method - DecodeMetadataIndex(ed.declaringType->methodStart) - 1;
#endif
			}
			if (semantics & (uint16_t)MethodSemanticsAttributes::RemoveOn)
			{
				EventDetail& ed = _events[propertyOrEventIndex];
				if (ed.removeMethodIndex != 0)
				{
					RaiseBadImageException("event has multiple remove methods");
				}
				ed.removeMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				ed.il2cppDefinition.remove = method - DecodeMetadataIndex(ed.declaringType->methodStart) - 1;
#endif
			}
			if (semantics & (uint16_t)MethodSemanticsAttributes::Fire)
			{
				EventDetail& ed = _events[propertyOrEventIndex];
				if (ed.fireMethodIndex != 0)
				{
					RaiseBadImageException("event has multiple fire methods");
				}
				ed.fireMethodIndex = method;
#if HYBRIDCLR_UNITY_2019
				ed.il2cppDefinition.raise = method - DecodeMetadataIndex(ed.declaringType->methodStart) - 1;
#endif
			}
		}
	}

	struct EnclosingClassInfo
	{
		uint32_t enclosingTypeIndex; // rowIndex - 1
		std::vector<uint32_t> nestedTypeIndexs;
	};

	void InterpreterImage::InitNestedClass()
	{
		const Table& nestedClassTb = _rawImage->GetTable(TableType::NESTEDCLASS);
		_nestedTypeDefineIndexs.reserve(nestedClassTb.rowNum);
		std::vector<EnclosingClassInfo> enclosingTypes;
		const uint32_t noEnclosingType = std::numeric_limits<uint32_t>::max();
		std::vector<uint32_t> enclosingTypeByNestedType(_typesDefines.size(), noEnclosingType);
		uint32_t lastNestedClass = 0;

		for (uint32_t i = 0; i < nestedClassTb.rowNum; i++)
		{
			TbNestedClass data = _rawImage->ReadNestedClass(i + 1);
			if (data.nestedClass == 0 || data.nestedClass > _typesDefines.size() ||
				data.enclosingClass == 0 || data.enclosingClass > _typesDefines.size())
			{
				RaiseBadImageException("nested class row index is out of range");
			}
			if (data.nestedClass <= lastNestedClass)
			{
				RaiseBadImageException("nested class rows are duplicated or not sorted");
			}
			if (data.nestedClass == data.enclosingClass)
			{
				RaiseBadImageException("type cannot enclose itself");
			}
			enclosingTypeByNestedType[data.nestedClass - 1] = data.enclosingClass - 1;
			Il2CppTypeDefinition& nestedType = _typesDefines[data.nestedClass - 1];
			Il2CppTypeDefinition& enclosingType = _typesDefines[data.enclosingClass - 1];
			if (enclosingType.nested_type_count == 0)
			{
				// 此行代码不能删，用于标识 enclosingTypes的index
				enclosingType.nestedTypesStart = (uint32_t)enclosingTypes.size();
				enclosingTypes.push_back({ data.enclosingClass - 1 });
			}
			if (enclosingType.nested_type_count == std::numeric_limits<uint16_t>::max())
			{
				RaiseBadImageException("nested type count exceeds runtime limits");
			}
			++enclosingType.nested_type_count;
			enclosingTypes[enclosingType.nestedTypesStart].nestedTypeIndexs.push_back(data.nestedClass - 1);
			nestedType.declaringTypeIndex = enclosingType.byvalTypeIndex;
			lastNestedClass = data.nestedClass;
		}

		std::vector<uint8_t> visitState(_typesDefines.size(), 0);
		for (uint32_t typeIndex = 0; typeIndex < _typesDefines.size(); ++typeIndex)
		{
			uint32_t current = typeIndex;
			while (current != noEnclosingType && visitState[current] == 0)
			{
				visitState[current] = 1;
				current = enclosingTypeByNestedType[current];
			}
			if (current != noEnclosingType && visitState[current] == 1)
			{
				RaiseBadImageException("nested class hierarchy contains a cycle");
			}
			current = typeIndex;
			while (current != noEnclosingType && visitState[current] == 1)
			{
				visitState[current] = 2;
				current = enclosingTypeByNestedType[current];
			}
		}

		for (auto& enclosingType : enclosingTypes)
		{
			Il2CppTypeDefinition& enclosingTypeDef = _typesDefines[enclosingType.enclosingTypeIndex];
			IL2CPP_ASSERT(enclosingType.nestedTypeIndexs.size() == (size_t)enclosingTypeDef.nested_type_count);
			enclosingTypeDef.nestedTypesStart = (NestedTypeIndex)_nestedTypeDefineIndexs.size();
			enclosingTypeDef.nested_type_count = (uint16_t)enclosingType.nestedTypeIndexs.size();
			_nestedTypeDefineIndexs.insert(_nestedTypeDefineIndexs.end(), enclosingType.nestedTypeIndexs.begin(), enclosingType.nestedTypeIndexs.end());
		}
	}

	void InterpreterImage::InitClassLayouts0()
	{
		const Table& classLayoutTb = _rawImage->GetTable(TableType::CLASSLAYOUT);
		for (uint32_t i = 0; i < classLayoutTb.rowNum; i++)
		{
			TbClassLayout data = _rawImage->ReadClassLayout(i + 1);
			if (data.parent == 0 || data.parent > _typesDefines.size())
			{
				RaiseBadImageException("class layout parent index is out of range");
			}
			if (data.packingSize != 0 &&
				(data.packingSize > 128 || (data.packingSize & (data.packingSize - 1)) != 0))
			{
				RaiseBadImageException("class layout packing size is invalid");
			}
			uint32_t typeIndex = data.parent - 1;
			if (_classLayouts.find(typeIndex) != _classLayouts.end())
			{
				RaiseBadImageException("type has multiple class layout rows");
			}
			if (data.classSize > std::numeric_limits<uint32_t>::max() - sizeof(Il2CppObject))
			{
				RaiseBadImageException("class layout size exceeds runtime limits");
			}
			_classLayouts.emplace(typeIndex, data);
			if (data.classSize > 0)
			{
				Il2CppTypeDefinitionSizes& typeSizes = _typeDetails[typeIndex].typeSizes;
				typeSizes.instance_size = data.classSize + sizeof(Il2CppObject);
			}
		}
	}

	void InterpreterImage::InitClassLayouts()
	{
		ClassFieldLayoutCalculator calculator(this);
		for (Il2CppTypeDefinition& type : _typesDefines)
		{
			const Il2CppType* il2cppType = GetIl2CppTypeFromTypeDefinition(&type);
			calculator.CalcClassNotStaticFields(il2cppType);
		}

		for (TypeDefinitionDetail& type : _typeDetails)
		{
			const Il2CppTypeDefinition* typeDef = GetTypeDefinitionByTypeDetail(&type);
			const Il2CppType* il2cppType = GetIl2CppTypeFromTypeDefinition(typeDef);
			ClassLayoutInfo* layout = calculator.GetClassLayoutInfo(il2cppType);
			calculator.CalcClassStaticFields(il2cppType);

			auto& sizes = type.typeSizes;
			sizes.native_size = layout->nativeSize;
			if (typeDef->genericContainerIndex == kGenericContainerIndexInvalid)
			{
				sizes.static_fields_size = layout->staticFieldsSize;
				sizes.thread_static_fields_size = layout->threadStaticFieldsSize;
			}
			else
			{
				sizes.static_fields_size = 0;
				sizes.thread_static_fields_size = 0;
			}
			if (sizes.instance_size == 0)
			{
				sizes.instance_size = layout->instanceSize;
			}
			int32_t fieldStart = DecodeMetadataIndex(typeDef->fieldStart);
			for (int32_t i = 0, end = typeDef->field_count; i < end ; i++)
			{
				FieldDetail& fd = _fieldDetails[fieldStart + i];
				FieldLayout& fieldLayout = layout->fields[i];
				if (fd.offset == 0)
				{
					fd.offset = fieldLayout.offset;
				}
				else if (fd.offset == THREAD_LOCAL_STATIC_MASK)
				{
					fd.offset = fieldLayout.offset;
				}
				else
				{
					IL2CPP_ASSERT(fd.offset == fieldLayout.offset);
					int a = 0;
				}
			}
		}
	}

	void InterpreterImage::InitClassLayoutsLazy()
	{
		_classLayoutInitialized.assign(_typesDefines.size(), 0);
		_classLayoutInitializableTypeCount = (uint32_t)_typesDefines.size();
		if (!_classLayoutInitialized.empty())
		{
			// <Module> is not returned by Assembly.GetTypes and has no runtime layout.
			_classLayoutInitialized[0] = 1;
			_classLayoutInitializedTypeCount = 1;
		}
		if (!_classLayoutCalculator)
		{
			_classLayoutCalculator = new ClassFieldLayoutCalculator(this);
		}
	}

	void InterpreterImage::InitClassLayout(uint32_t index)
	{
		IL2CPP_ASSERT(index < _typesDefines.size());
		if (_classLayoutInitialized[index])
		{
			return;
		}
		IL2CPP_ASSERT(_classLayoutCalculator);

		Il2CppTypeDefinition* typeDef = &_typesDefines[index];
		const Il2CppType* il2cppType = GetIl2CppTypeFromTypeDefinition(typeDef);
		// ThreadStatic markers are only relevant to field-bearing types. Avoid
		// scanning the entire custom-attribute table when the first touched type
		// has no fields (the common static-class entry case).
		if (typeDef->field_count != 0)
			EnsureThreadStaticFieldsInitializedLocked();
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		auto stageStart = std::chrono::steady_clock::now();
#endif
		_classLayoutCalculator->CalcClassNotStaticFields(il2cppType);
		_classLayoutCalculator->CalcClassStaticFields(il2cppType);
		ClassLayoutInfo* layout = _classLayoutCalculator->GetClassLayoutInfo(il2cppType);
		IL2CPP_ASSERT(layout);
		if (il2cppType->type == IL2CPP_TYPE_VALUETYPE)
		{
			const uint32_t blittableBit = 1u << (il2cpp::vm::kBitIsBlittable - 1);
			if (layout->blittable)
			{
				typeDef->bitfield |= blittableBit;
			}
			else
			{
				typeDef->bitfield &= ~blittableBit;
			}
		}

		auto& sizes = _typeDetails[index].typeSizes;
		sizes.native_size = layout->nativeSize;
		if (typeDef->genericContainerIndex == kGenericContainerIndexInvalid)
		{
			sizes.static_fields_size = layout->staticFieldsSize;
			sizes.thread_static_fields_size = layout->threadStaticFieldsSize;
		}
		else
		{
			sizes.static_fields_size = 0;
			sizes.thread_static_fields_size = 0;
		}
		if (sizes.instance_size == 0)
		{
			sizes.instance_size = layout->instanceSize;
		}

		int32_t fieldStart = DecodeMetadataIndex(typeDef->fieldStart);
		for (int32_t i = 0, end = typeDef->field_count; i < end; i++)
		{
			FieldDetail& fd = _fieldDetails[fieldStart + i];
			FieldLayout& fieldLayout = layout->fields[i];
			if (fd.offset == 0 || fd.offset == THREAD_LOCAL_STATIC_MASK)
			{
				fd.offset = fieldLayout.offset;
			}
			else
			{
				IL2CPP_ASSERT(fd.offset == fieldLayout.offset);
			}
		}
		_classLayoutInitialized[index] = 1;
		if (++_classLayoutInitializedTypeCount == _classLayoutInitializableTypeCount)
		{
			delete _classLayoutCalculator;
			_classLayoutCalculator = nullptr;
		}
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		RecordMetadataInitStage(_index, "LazyClassLayout", (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stageStart).count());
#endif
	}

	bool InterpreterImage::TryApplyClassLayoutLocked(Il2CppClass* klass)
	{
		// The calculator is a temporary cache owned by the lazy layout phase. Keep
		// this fast path deliberately narrow: generic, explicit-size, and static
		// field layouts still use IL2CPP's canonical layout code.
		if (!_classLayoutCalculator || !klass || klass->generic_class
			|| klass->byval_arg.type != IL2CPP_TYPE_CLASS || klass->field_count == 0
			|| (klass->flags & TYPE_ATTRIBUTE_EXPLICIT_LAYOUT) != 0
			|| !il2cpp::vm::MetadataCache::StructLayoutSizeIsDefault(klass->typeMetadataHandle))
		{
			return false;
		}

		const Il2CppTypeDefinition* typeDef = reinterpret_cast<const Il2CppTypeDefinition*>(klass->typeMetadataHandle);
		const Il2CppType* type = GetIl2CppTypeFromTypeDefinition(typeDef);
		ClassLayoutInfo* layout = _classLayoutCalculator->GetClassLayoutInfo(type);
		if (!layout || layout->fields.size() != klass->field_count || layout->hasStaticFields)
		{
			return false;
		}

		// SetupFieldsFromDefinitionLocked has already materialized FieldInfo objects.
		// Reuse the offsets and sizes computed by the interpreter metadata path and
		// avoid running the same FieldLayout traversal a second time.
		klass->has_references = klass->parent ? klass->parent->has_references : false;
		for (uint16_t i = 0; i < klass->field_count; ++i)
		{
			FieldInfo* field = klass->fields + i;
			const FieldLayout& fieldLayout = layout->fields[i];
			if (fieldLayout.isNormalStatic || fieldLayout.isThreadStatic)
			{
				return false;
			}
			field->offset = fieldLayout.offset;
			const Il2CppType* fieldType = il2cpp::vm::Type::GetUnderlyingType(field->type);
			if (il2cpp::vm::Type::IsReference(fieldType)
				|| (il2cpp::vm::Type::IsStruct(fieldType)
					&& il2cpp::vm::Class::HasReferences(il2cpp::vm::Class::FromIl2CppType(fieldType))))
			{
				klass->has_references = true;
			}
		}

		klass->instance_size = static_cast<uint32_t>(layout->instanceSize);
		klass->actualSize = static_cast<uint32_t>(layout->actualSize);
		klass->native_size = layout->nativeSize;
		klass->minimumAlignment = layout->alignment;
		klass->static_fields_size = 0;
		klass->thread_static_fields_size = 0;
	#if HYBRIDCLR_UNITY_2022_OR_NEW
		klass->stack_slot_size = sizeof(void*);
	#endif
		return true;
	}

	void InterpreterImage::FreezeIl2CppTypeCache()
	{
		if (_type2Indexs.empty())
		{
			return;
		}

		// Once the load-time type graph is complete, retain only a compact sorted
		// index. Lazy metadata still needs to deduplicate against these entries,
		// while the unordered map's buckets and node overhead are no longer useful.
		_frozenTypeIndexes.reserve(_frozenTypeIndexes.size() + _type2Indexs.size());
		for (auto it = _type2Indexs.begin(); it != _type2Indexs.end(); ++it)
		{
			_frozenTypeIndexes.push_back({ it->first, it->second,
				static_cast<uint32_t>(Il2CppTypeHashShallow()(it->first)) });
		}
		std::sort(_frozenTypeIndexes.begin(), _frozenTypeIndexes.end(),
			[](const FrozenTypeIndex& lhs, const FrozenTypeIndex& rhs)
			{
				return lhs.hash < rhs.hash;
			});
		Il2CppHashMap<const Il2CppType*, uint32_t, Il2CppTypeHashShallow, Il2CppTypeEqualityComparerShallow> emptyTypeIndexes;
		_type2Indexs.swap(emptyTypeIndexes);
	}

	uint32_t InterpreterImage::AddIl2CppTypeCache(const Il2CppType* type)
	{
		const uint32_t hash = static_cast<uint32_t>(Il2CppTypeHashShallow()(type));
		auto frozen = std::lower_bound(_frozenTypeIndexes.begin(), _frozenTypeIndexes.end(), hash,
			[](const FrozenTypeIndex& entry, uint32_t value)
			{
				return entry.hash < value;
			});
		for (auto it = frozen; it != _frozenTypeIndexes.end() && it->hash == hash; ++it)
		{
			if (Il2CppTypeEqualityComparerShallow()(it->type, type))
			{
				return it->index;
			}
		}

		auto it = _type2Indexs.find(type);
		if (it != _type2Indexs.end())
		{
			return it->second;
		}
		uint32_t encodeIndex = EncodeWithIndex((uint32_t)_types.size());
		_types.push_back(type);
		_type2Indexs.insert({ type, encodeIndex });
		return encodeIndex;
	}

	const Il2CppType* InterpreterImage::GetIl2CppTypeFromRawIndex(uint32_t index) const
	{
		return _types[index];
	}

	const Il2CppType* InterpreterImage::GetGenericParameterConstraintFromIndex(GenericParameterConstraintIndex index)
	{
		IL2CPP_ASSERT((size_t)index < _genericConstraints.size());
		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);
		TypeIndex typeIndex = _genericConstraints[index];
		if (typeIndex == kTypeIndexInvalid)
		{
			TbGenericParamConstraint data = _rawImage->ReadGenericParamConstraint(index + 1);
			Il2CppGenericParameter& genericParam = _genericParams[data.owner - 1];
			const Il2CppGenericContainer* klassGc;
			const Il2CppGenericContainer* methodGc;
			GetClassAndMethodGenericContainerFromGenericContainerIndex(genericParam.ownerIndex, klassGc, methodGc);
			const Il2CppType* paramCons = ReadTypeFromToken(klassGc, methodGc,
				DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.constraint), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.constraint));
			_genericConstraints[index] = typeIndex = DecodeMetadataIndex(AddIl2CppTypeCache(paramCons));
		}
		return _types[typeIndex];
	}

	uint32_t InterpreterImage::AddIl2CppGenericContainers(Il2CppGenericContainer& geneContainer)
	{
		uint32_t index = (uint32_t)_genericContainers.size();
		_genericContainers.push_back(geneContainer);
		return EncodeWithIndex(index);
	}

	void InterpreterImage::InitClass()
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		_classList.resize(typeDefTb.rowNum);
	}

	Il2CppClass* InterpreterImage::GetTypeInfoFromTypeDefinitionRawIndex(uint32_t index)
	{
		IL2CPP_ASSERT(index < _classList.size());
		Il2CppClass* klass = il2cpp::os::Atomic::LoadPointerAcquire(&_classList[index]);
		if (klass)
		{
			return klass;
		}
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		klass = il2cpp::os::Atomic::LoadPointerAcquire(&_classList[index]);
		if (klass)
		{
			return klass;
		}
		InitClassLayout(index);
		Il2CppTypeDefinition& typeDef = _typesDefines[index];
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		if (!IsInterface(typeDef.flags) && typeDef.interfaceOffsetsStart == 0)
		{
			auto stageStart = std::chrono::steady_clock::now();
			EnsureVTableInitializedLocked(&typeDef);
			RecordMetadataInitStage(_index, "LazyVTable", (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - stageStart).count());
		}
#else
		EnsureVTableInitializedLocked(&typeDef);
#endif
		klass = il2cpp::vm::GlobalMetadata::FromTypeDefinition(EncodeWithIndex(index));
		IL2CPP_ASSERT(klass->interfaces_count <= klass->interface_offsets_count || _typesDefines[index].interfaceOffsetsStart == 0);
		il2cpp::os::Atomic::PublishPointer(&_classList[index], klass);
		return klass;
	}

	const Il2CppType* InterpreterImage::GetInterfaceFromGlobalOffset(TypeInterfaceIndex globalOffset)
	{
		IL2CPP_ASSERT((uint32_t)globalOffset < (uint32_t)_interfaceDefines.size());
		il2cpp::os::FastAutoLock metaLock(&il2cpp::vm::g_MetadataLock);

		TypeIndex typeIndex = _interfaceDefines[globalOffset];
		if (typeIndex == kTypeIndexInvalid)
		{
			uint32_t rowIndex = globalOffset + 1;
			TbInterfaceImpl data = _rawImage->ReadInterfaceImpl(rowIndex);
			Il2CppTypeDefinition& typeDef = _typesDefines[data.classIdx - 1];
			const Il2CppType* intType = ReadTypeFromToken(GetGenericContainerByTypeDefinition(&typeDef), nullptr,
				DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.interfaceIdx), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.interfaceIdx));
			_interfaceDefines[globalOffset] = typeIndex = DecodeMetadataIndex(AddIl2CppTypeCache(intType));
		}

		return _types[typeIndex];
	}

	const Il2CppType* InterpreterImage::GetInterfaceFromIndex(const Il2CppClass* klass, TypeInterfaceIndex globalOffset)
	{
		return GetInterfaceFromGlobalOffset(globalOffset);
	}

	const Il2CppType* InterpreterImage::GetInterfaceFromOffset(const Il2CppClass* klass, TypeInterfaceIndex offset)
	{
		const Il2CppTypeDefinition* typeDef = (const Il2CppTypeDefinition*)(klass->typeMetadataHandle);
		IL2CPP_ASSERT(typeDef);
		return GetInterfaceFromOffset(typeDef, offset);
	}

	const Il2CppType* InterpreterImage::GetInterfaceFromOffset(const Il2CppTypeDefinition* typeDef, TypeInterfaceIndex offset)
	{
		uint32_t globalOffset = typeDef->interfacesStart + offset;
		return GetInterfaceFromGlobalOffset(globalOffset);
	}

	Il2CppInterfaceOffsetInfo InterpreterImage::GetInterfaceOffsetInfo(const Il2CppTypeDefinition* typeDefine, TypeInterfaceOffsetIndex index)
	{
		uint32_t globalIndex = DecodeMetadataIndex((uint32_t)(typeDefine->interfaceOffsetsStart + index));
		IL2CPP_ASSERT(globalIndex < (uint32_t)_interfaceOffsets.size());

		InterfaceOffsetInfo& offsetPair = _interfaceOffsets[globalIndex];
		return { offsetPair.type, (int32_t)offsetPair.offset };
	}

	Il2CppClass* InterpreterImage::GetNestedTypeFromOffset(const Il2CppTypeDefinition* typeDefine, TypeNestedTypeIndex offset)
	{
		uint32_t globalIndex = typeDefine->nestedTypesStart + offset;
		IL2CPP_ASSERT(globalIndex < (uint32_t)_nestedTypeDefineIndexs.size());
		uint32_t typeDefIndex = _nestedTypeDefineIndexs[globalIndex];
		IL2CPP_ASSERT(typeDefIndex < (uint32_t)_typesDefines.size());
		return il2cpp::vm::GlobalMetadata::GetTypeInfoFromHandle((Il2CppMetadataTypeHandle)&_typesDefines[typeDefIndex]);
	}

	Il2CppClass* InterpreterImage::GetNestedTypeFromOffset(const Il2CppClass* klass, TypeNestedTypeIndex offset)
	{
		return GetNestedTypeFromOffset((Il2CppTypeDefinition*)klass->typeMetadataHandle, offset);
	}

	Il2CppTypeDefinition* InterpreterImage::GetNestedTypes(Il2CppTypeDefinition* typeDefinition, void** iter)
	{
		if (_nestedTypeDefineIndexs.empty())
		{
			return nullptr;
		}
		const TypeDefinitionIndex* nestedTypeIndices = (const TypeDefinitionIndex*)(&_nestedTypeDefineIndexs[typeDefinition->nestedTypesStart]);

		if (!*iter)
		{
			if (typeDefinition->nested_type_count == 0)
				return NULL;

			*iter = (void*)(nestedTypeIndices);
			return &_typesDefines[nestedTypeIndices[0]];
		}

		TypeDefinitionIndex* nestedTypeAddress = (TypeDefinitionIndex*)*iter;
		nestedTypeAddress++;
		ptrdiff_t index = nestedTypeAddress - nestedTypeIndices;

		if (index < typeDefinition->nested_type_count)
		{
			*iter = nestedTypeAddress;
			return &_typesDefines[*nestedTypeAddress];
		}

		return NULL;
	}

	const Il2CppAssembly* InterpreterImage::GetReferencedAssembly(int32_t referencedAssemblyTableIndex, const Il2CppAssembly assembliesTable[], int assembliesCount)
	{
		auto& table = _rawImage->GetTable(TableType::ASSEMBLYREF);
		IL2CPP_ASSERT((uint32_t)referencedAssemblyTableIndex < table.rowNum);

		TbAssemblyRef assRef = _rawImage->ReadAssemblyRef(referencedAssemblyTableIndex + 1);
		const char* refAssName = _rawImage->GetStringFromRawIndex(assRef.name);
		const Il2CppAssembly* il2cppAssRef = il2cpp::vm::Assembly::GetLoadedAssembly(refAssName);
		if (!il2cppAssRef)
		{
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetDllNotFoundException(refAssName));
		}
		return il2cppAssRef;
	}

	void InterpreterImage::ReadFieldRefInfoFromFieldDefToken(uint32_t rowIndex, FieldRefInfo& ret)
	{
		IL2CPP_ASSERT(rowIndex > 0);
		EnsureFieldMetadataInitializedLocked(rowIndex - 1);
		const FieldDetail& fd = GetFieldDetailFromRawIndex(rowIndex - 1);
		ret.containerType = GetRawTypeDefinitionType(DecodeMetadataIndex(fd.typeDefIndex));
		ret.field = &fd.fieldDef;
	}

	void InterpreterImage::GetClassAndMethodGenericContainerFromGenericContainerIndex(GenericContainerIndex idx, const Il2CppGenericContainer*& klassGc, const Il2CppGenericContainer*& methodGc)
	{
		Il2CppGenericContainer* gc = GetGenericContainerByRawIndex(DecodeMetadataIndex(idx));
		IL2CPP_ASSERT(gc);
		if (gc->is_method)
		{
			const Il2CppMethodDefinition* methodDef = GetMethodDefinitionFromRawIndex(DecodeMetadataIndex(gc->ownerIndex));
			klassGc = GetGenericContainerByTypeDefRawIndex(DecodeMetadataIndex(methodDef->declaringType));
			methodGc = GetGenericContainerByRawIndex(DecodeMetadataIndex(methodDef->genericContainerIndex));
		}
		else
		{
			klassGc = gc;
			methodGc = nullptr;
		}
	}

	void InterpreterImage::InitGenericParamConstraintDefs()
	{
		const Table& tb = _rawImage->GetTable(TableType::GENERICPARAMCONSTRAINT);
		_genericConstraints.resize(tb.rowNum, kTypeIndexInvalid);
		uint32_t lastOwner = 0;
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			uint32_t rowIndex = i + 1;
			TbGenericParamConstraint data = _rawImage->ReadGenericParamConstraint(rowIndex);
			if (data.owner == 0 || data.owner > _genericParams.size())
			{
				RaiseBadImageException("generic parameter constraint owner is out of range");
			}
			ValidateTypeDefOrRefOrSpecCodedIndex(*_rawImage, data.constraint,
				"generic parameter constraint type is invalid");
			Il2CppGenericParameter& genericParam = _genericParams[data.owner - 1];

			if (genericParam.constraintsCount == 0)
			{
				genericParam.constraintsStart = EncodeWithIndex(i);
			}
			else if (data.owner != lastOwner)
			{
				RaiseBadImageException("generic parameter constraints for one owner are not contiguous");
			}
			if (genericParam.constraintsCount == std::numeric_limits<int16_t>::max())
			{
				RaiseBadImageException("generic parameter constraint count exceeds runtime limits");
			}
			++genericParam.constraintsCount;
			lastOwner = data.owner;
			//_genericConstraints[i] == kTypeIndexInvalid;

			//Il2CppType paramCons = {};

			//const Il2CppGenericContainer* klassGc;
			//const Il2CppGenericContainer* methodGc;
			//GetClassAndMethodGenericContainerFromGenericContainerIndex(genericParam.ownerIndex, klassGc, methodGc);

			//ReadTypeFromToken(klassGc, methodGc, DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.constraint), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.constraint), paramCons);
			//_genericConstraints[i] = DecodeMetadataIndex(AddIl2CppTypeCache(paramCons));
		}
	}

	void InterpreterImage::InitGenericParamDefs0()
	{
		const Table& tb = _rawImage->GetTable(TableType::GENERICPARAM);
		_genericParams.resize(tb.rowNum);
	}

	void InterpreterImage::InitGenericParamDefs()
	{
		const Table& tb = _rawImage->GetTable(TableType::GENERICPARAM);
		uint32_t lastOwner = 0;
		for (uint32_t i = 0; i < tb.rowNum; i++)
		{
			uint32_t rowIndex = i + 1;
			TbGenericParam data = _rawImage->ReadGenericParam(rowIndex);
			Il2CppGenericParameter& paramDef = _genericParams[i];
			paramDef.num = data.number;
			paramDef.flags = data.flags;
			paramDef.nameIndex = EncodeWithIndex(data.name);
			// constraintsStart 和 constrantsCount init at InitGenericParamConstrains() latter

			TableType ownerType = DecodeTypeOrMethodDefCodedIndexTableType(data.owner);
			uint32_t ownerIndex = DecodeTypeOrMethodDefCodedIndexRowIndex(data.owner);
			const uint32_t ownerCount = ownerType == TableType::TYPEDEF
				? static_cast<uint32_t>(_typesDefines.size())
				: static_cast<uint32_t>(_methodDefines.size());
			if (ownerIndex == 0 || ownerIndex > ownerCount)
			{
				RaiseBadImageException("generic parameter owner is out of range");
			}
			Il2CppGenericContainer* geneContainer;
			int32_t interIndex = ownerIndex - 1;
			if (ownerType == TableType::TYPEDEF)
			{
				Il2CppTypeDefinition& typeDef = _typesDefines[interIndex];
				if (typeDef.genericContainerIndex == kGenericContainerIndexInvalid)
				{
					Il2CppGenericContainer c = {};
					c.ownerIndex = EncodeWithIndex(interIndex);
					c.is_method = false;
					typeDef.genericContainerIndex = AddIl2CppGenericContainers(c);
				}
				geneContainer = &_genericContainers[DecodeMetadataIndex(typeDef.genericContainerIndex)];
				paramDef.ownerIndex = typeDef.genericContainerIndex;
			}
			else
			{
				Il2CppMethodDefinition& methodDef = _methodDefines[interIndex];
				if (methodDef.genericContainerIndex == kGenericContainerIndexInvalid)
				{
					Il2CppGenericContainer c = {};
					c.ownerIndex = EncodeWithIndex(interIndex);
					c.is_method = true;
					methodDef.genericContainerIndex = AddIl2CppGenericContainers(c);
				}
				geneContainer = &_genericContainers[DecodeMetadataIndex(methodDef.genericContainerIndex)];
				paramDef.ownerIndex = methodDef.genericContainerIndex;
			}
			if (geneContainer->type_argc == 0)
			{
				geneContainer->genericParameterStart = EncodeWithIndex(i);
			}
			else if (data.owner != lastOwner)
			{
				RaiseBadImageException("generic parameters for one owner are not contiguous");
			}
			if (data.number != static_cast<uint32_t>(geneContainer->type_argc))
			{
				RaiseBadImageException("generic parameter number is not sequential");
			}
			++geneContainer->type_argc;
			lastOwner = data.owner;
		}
	}


	void InterpreterImage::InitInterfaces()
	{
		const Table& table = _rawImage->GetTable(TableType::INTERFACEIMPL);

		// interface中只包含直接继承的interface,不包括来自父类的
		// 此interface只在CastClass及Type.GetInterfaces()反射函数中
		// 发挥作用，不在callvir中发挥作用。
		// interfaceOffsets中包含了水平展开的所有interface(包括父类的)
		_interfaceDefines.resize(table.rowNum, kTypeIndexInvalid);
		uint32_t lastClassIdx = 0;
		for (uint32_t i = 0; i < table.rowNum; i++)
		{
			uint32_t rowIndex = i + 1;
			TbInterfaceImpl data = _rawImage->ReadInterfaceImpl(rowIndex);
			if (data.classIdx == 0 || data.classIdx > _typesDefines.size())
			{
				RaiseBadImageException("interface implementation class index is out of range");
			}
			ValidateTypeDefOrRefOrSpecCodedIndex(*_rawImage, data.interfaceIdx,
				"interface implementation type is invalid");

			Il2CppTypeDefinition& typeDef = _typesDefines[data.classIdx - 1];
			//Il2CppType intType = {};
			//ReadTypeFromToken(GetGenericContainerByTypeDefinition(&typeDef), nullptr,
			//	DecodeTypeDefOrRefOrSpecCodedIndexTableType(data.interfaceIdx), DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(data.interfaceIdx), intType);
			//_interfaceDefines[i] = DecodeMetadataIndex(AddIl2CppTypeCache(intType));
			if (typeDef.interfaces_count == 0)
			{
				typeDef.interfacesStart = (InterfacesIndex)i;
			}
			else
			{
				// 必须连续
				if (data.classIdx != lastClassIdx)
				{
					RaiseBadImageException("interface implementations for one type are not contiguous");
				}
			}
			if (typeDef.interfaces_count == std::numeric_limits<uint16_t>::max())
			{
				RaiseBadImageException("type interface count exceeds runtime limits");
			}
			++typeDef.interfaces_count;
			lastClassIdx = data.classIdx;
		}
	}

	void InterpreterImage::ComputeVTable(TypeDefinitionDetail* tdd)
	{
		Il2CppTypeDefinition& typeDef = *GetTypeDefinitionByTypeDetail(tdd);
		if (IsInterface(typeDef.flags) || typeDef.interfaceOffsetsStart != 0)
		{
			return;
		}

		// A non-generic derived type with no virtual/interface/method-impl additions
		// has exactly the parent's published vtable. Reuse the parent's stable slab
		// instead of rebuilding and copying an identical table for every such type.
		if (typeDef.parentIndex != kInvalidIndex
			&& typeDef.genericContainerIndex == kGenericContainerIndexInvalid
			&& typeDef.interfaces_count == 0
			&& !HasMethodImpls(&typeDef))
		{
			const Il2CppType* parentType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(typeDef.parentIndex);
			const Il2CppTypeDefinition* parentTypeDef = GetUnderlyingTypeDefinition(parentType);
			const bool hasVirtualMethod = _typeDetails[GetTypeRawIndex(&typeDef)].virtualMethodCount != 0;
			InterpreterImage* parentImage = IsInterpreterType(parentTypeDef) ? MetadataModule::GetImage(parentTypeDef) : nullptr;
			if (!hasVirtualMethod && parentImage == this)
			{
				if (parentTypeDef->interfaceOffsetsStart == 0)
				{
					EnsureVTableInitializedLocked(parentTypeDef);
				}
				uint32_t parentVtableCount = 0;
				const VirtualMethodImpl* parentVtable = GetPublishedVTable(parentTypeDef, parentVtableCount);
				tdd->vtable = const_cast<VirtualMethodImpl*>(parentVtable);
				tdd->vtableCount = parentVtableCount;
				typeDef.vtableStart = parentTypeDef->vtableStart;
				typeDef.vtable_count = parentTypeDef->vtable_count;
				typeDef.interfaceOffsetsStart = parentTypeDef->interfaceOffsetsStart;
				typeDef.interface_offsets_count = parentTypeDef->interface_offsets_count;
				++_vtableInitializedTypeCount;
				IL2CPP_ASSERT(_vtableInitializedTypeCount <= _vtableInitializableTypeCount);
				return;
			}
		}

		if (typeDef.parentIndex != kInvalidIndex)
		{
			const Il2CppType* parentType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(typeDef.parentIndex);
			const Il2CppTypeDefinition* parentTypeDef = GetUnderlyingTypeDefinition(parentType);
			if (IsInterpreterType(parentTypeDef) && parentTypeDef->interfaceOffsetsStart == 0)
			{
				MetadataModule::GetImage(parentTypeDef)->EnsureVTableInitializedLocked(parentTypeDef);
			}
		}

		const Il2CppType* type = GetIl2CppTypeFromRawIndex(DecodeMetadataIndex(typeDef.byvalTypeIndex));
		VTableSetUp* typeTree = VTableSetUp::BuildByType(_cacheTrees, type);

		uint32_t offsetsStart = (uint32_t)_interfaceOffsets.size();

		auto& vms = typeTree->GetVirtualMethodImpls();
		if (vms.empty())
		{
			tdd->vtable = nullptr;
			tdd->vtableCount = 0;
		}
		else
		{
			tdd->vtable = (VirtualMethodImpl*)HYBRIDCLR_METADATA_CALLOC(vms.size(), sizeof(VirtualMethodImpl));
			tdd->vtableCount = (uint32_t)vms.size();
			std::memcpy(tdd->vtable, &vms[0], vms.size() * sizeof(VirtualMethodImpl));
		}

		auto& interfaceOffsetInfos = typeTree->GetInterfaceOffsetInfos();
		for (auto ioi : interfaceOffsetInfos)
		{
			_interfaceOffsets.push_back({ ioi.type, ioi.offset });
		}

		typeDef.vtableStart = EncodeWithIndex(0);
		typeDef.vtable_count = (uint16_t)vms.size();
		typeDef.interfaceOffsetsStart = EncodeWithIndex(offsetsStart);
		typeDef.interface_offsets_count = (uint16_t)interfaceOffsetInfos.size();
		++_vtableInitializedTypeCount;
		IL2CPP_ASSERT(_vtableInitializedTypeCount <= _vtableInitializableTypeCount);

		Il2CppClass* klass = _classList[GetTypeRawIndex(&typeDef)];
		IL2CPP_ASSERT(!klass);

		if (_vtableInitializedTypeCount == _vtableInitializableTypeCount)
		{
			for (VTableSetUp* tree : _vtableTreesByTypeDefinition)
			{
				if (tree)
				{
					tree->~VTableSetUp();
					HYBRIDCLR_FREE(tree);
				}
			}
			std::vector<VTableSetUp*> emptyDirectCache;
			_vtableTreesByTypeDefinition.swap(emptyDirectCache);
			for (auto& entry : _cacheTrees)
			{
				entry.second->~VTableSetUp();
				HYBRIDCLR_FREE(entry.second);
			}
			Il2CppType2TypeDeclaringTreeMap emptyCache;
			_cacheTrees.swap(emptyCache);
		}
	}

	void InterpreterImage::EnsureVTableInitializedLocked(const Il2CppTypeDefinition* typeDef)
	{
		IL2CPP_ASSERT(typeDef);
		if (IsInterface(typeDef->flags) || typeDef->interfaceOffsetsStart != 0)
		{
			return;
		}
		uint32_t index = GetTypeRawIndex(typeDef);
		IL2CPP_ASSERT(index < _typeDetails.size());
		ComputeVTable(&_typeDetails[index]);
	}

	void InterpreterImage::InitVTables()
	{
		_vtableTreesByTypeDefinition.assign(_typesDefines.size(), nullptr);
		_vtableInitializableTypeCount = 0;
		// <Module> is never materialized, and interfaces do not enter ComputeVTable.
		for (uint32_t index = 1; index < (uint32_t)_typesDefines.size(); ++index)
		{
			const Il2CppTypeDefinition& typeDef = _typesDefines[index];
			if (!IsInterface(typeDef.flags) && typeDef.interfaceOffsetsStart == 0)
			{
				++_vtableInitializableTypeCount;
			}
		}
	}

	// index => MethodDefinition -> DeclaringClass -> index - klass->methodStart -> MethodInfo*
	const MethodInfo* InterpreterImage::GetMethodInfoFromMethodDefinitionRawIndex(uint32_t index)
	{
		IL2CPP_ASSERT((size_t)index <= _methodDefines.size());
		const Il2CppMethodDefinition* methodDefinition = GetMethodDefinitionFromRawIndex(index);
		// Interpreter method definitions carry an image-local declaring type
		// index. Resolve it directly instead of routing through the global
		// metadata handle/index conversion on every entry lookup.
		uint32_t declaringTypeIndex = DecodeMetadataIndex(methodDefinition->declaringType);
		IL2CPP_ASSERT(declaringTypeIndex < _typesDefines.size());
		const Il2CppTypeDefinition* typeDefinition = &_typesDefines[declaringTypeIndex];
		int32_t indexInClass = index - DecodeMetadataIndex(typeDefinition->methodStart);
		IL2CPP_ASSERT(indexInClass >= 0 && indexInClass < typeDefinition->method_count);
		Il2CppClass* klass = GetTypeInfoFromTypeDefinitionRawIndex(declaringTypeIndex);
#if UNITY_ENGINE_TUANJIE
		// Tuanjie exposes a lock-protected per-slot initializer. Entry lookup only
		// needs the requested MethodInfo; reflection enumeration still calls
		// Class::SetupMethods and materializes the complete method array.
		return il2cpp::vm::Class::GetOrSetupOneMethod(klass, static_cast<MethodIndex>(indexInClass));
#else
		il2cpp::vm::Class::SetupMethods(klass);
		return klass->methods[indexInClass];
#endif
	}

	const MethodInfo* InterpreterImage::GetMethodInfoFromMethodDefinition(const Il2CppMethodDefinition* methodDef)
	{
		uint32_t rawIndex = (uint32_t)(methodDef - &_methodDefines[0]);
		IL2CPP_ASSERT(rawIndex < (uint32_t)_methodDefines.size());
		return GetMethodInfoFromMethodDefinitionRawIndex(rawIndex);
	}

	// typeDef vTableSlot -> type virtual method index -> MethodDefinition*
	const Il2CppMethodDefinition* InterpreterImage::GetMethodDefinitionFromVTableSlot(const Il2CppTypeDefinition* typeDef, int32_t vTableSlot)
	{
		uint32_t typeDefIndex = GetTypeRawIndex(typeDef);
		IL2CPP_ASSERT(typeDefIndex < (uint32_t)_typeDetails.size());
		TypeDefinitionDetail& td = _typeDetails[typeDefIndex];

		IL2CPP_ASSERT(vTableSlot >= 0 && vTableSlot < (int32_t)td.vtableCount);
		VirtualMethodImpl& vmi = td.vtable[vTableSlot];
		return vmi.method;
	}

	const MethodInfo* InterpreterImage::GetMethodInfoFromVTableSlot(const Il2CppClass* klass, int32_t vTableSlot)
	{
		IL2CPP_ASSERT(!klass->generic_class);
		const Il2CppTypeDefinition* typeDef = (Il2CppTypeDefinition*)klass->typeMetadataHandle;
		//const Il2CppMethodDefinition* methodDef = GetMethodDefinitionFromVTableSlot((Il2CppTypeDefinition*)klass->typeMetadataHandle, vTableSlot);
		// FIX ME. why return null?
		//IL2CPP_ASSERT(methodDef);

		uint32_t typeDefIndex = GetTypeRawIndex(typeDef);
		IL2CPP_ASSERT(typeDefIndex < (uint32_t)_typeDetails.size());
		TypeDefinitionDetail& td = _typeDetails[typeDefIndex];

		IL2CPP_ASSERT(vTableSlot >= 0 && vTableSlot < (int32_t)td.vtableCount);
		VirtualMethodImpl& vmi = td.vtable[vTableSlot];
		if (vmi.method)
		{
			if (vmi.method->declaringType == EncodeWithIndex(typeDefIndex))
			{
				return il2cpp::vm::GlobalMetadata::GetMethodInfoFromMethodHandle((Il2CppMetadataMethodDefinitionHandle)vmi.method);
			}
			else
			{
				Il2CppClass* implClass = il2cpp::vm::Class::FromIl2CppType(vmi.type);
				IL2CPP_ASSERT(implClass != klass);
				il2cpp::vm::Class::SetupMethods(implClass);
				for (uint32_t i = 0; i < implClass->method_count; i++)
				{
					const MethodInfo* implMethod = implClass->methods[i];
					if (implMethod->token == vmi.method->token)
					{
						return implMethod;
					}
				}
				RaiseExecutionEngineException("not find vtable method");
			}
		}
		return nullptr;
	}

	Il2CppMethodPointer InterpreterImage::GetAdjustorThunk(uint32_t token)
	{
		uint32_t methodIndex = DecodeTokenRowIndex(token) - 1;
		IL2CPP_ASSERT(methodIndex < (uint32_t)_methodDefines.size());
		EnsureMethodMetadataInitialized(methodIndex);
		const Il2CppMethodDefinition* methodDef = &_methodDefines[methodIndex];
		return IsInstanceMethod(methodDef) ? hybridclr::interpreter::InterpreterModule::GetAdjustThunkMethodPointer(methodDef) : nullptr;
	}

	Il2CppMethodPointer InterpreterImage::GetMethodPointer(uint32_t token)
	{
		uint32_t methodIndex = DecodeTokenRowIndex(token) - 1;
		IL2CPP_ASSERT(methodIndex < (uint32_t)_methodDefines.size());
		EnsureMethodMetadataInitialized(methodIndex);
		const Il2CppMethodDefinition* methodDef = &_methodDefines[methodIndex];
		return hybridclr::interpreter::InterpreterModule::GetMethodPointer(methodDef);
	}

	InvokerMethod InterpreterImage::GetMethodInvoker(uint32_t token)
	{
		uint32_t methodIndex = DecodeTokenRowIndex(token) - 1;
		IL2CPP_ASSERT(methodIndex < (uint32_t)_methodDefines.size());
		EnsureMethodMetadataInitialized(methodIndex);
		const Il2CppMethodDefinition* methodDef = &_methodDefines[methodIndex];
		return hybridclr::interpreter::InterpreterModule::GetMethodInvoker(methodDef);
	}


	Il2CppString* InterpreterImage::ReadSerString(BlobReader& reader)
	{
		byte b = reader.PeekByte();
		if (b == 0xFF)
		{
			reader.SkipByte();
			return nullptr;
		}
		else if (b == 0)
		{
			reader.SkipByte();
			return il2cpp::vm::String::Empty();
		}
		else
		{
			uint32_t len = reader.ReadCompressedUint32();
#if !HYBRIDCLR_UNITY_2021_OR_NEW
			return il2cpp::vm::String::NewLen((char*)reader.GetAndSkipCurBytes(len), len);
#else
			char* chars = (char*)reader.GetDataOfReadPosition();
			reader.SkipBytes(len);
			return il2cpp::vm::String::NewLen(chars, len);
#endif
		}
	}

#if HYBRIDCLR_UNITY_2021_OR_NEW
	bool InterpreterImage::ReadUTF8SerString(BlobReader& reader, std::string& s)
	{
		byte b = reader.PeekByte();
		if (b == 0xFF)
		{
			reader.SkipByte();
			return false;
		}
		else if (b == 0)
		{
			reader.SkipByte();
			s.clear();
			return true;
		}
		else
		{
			uint32_t len = reader.ReadCompressedUint32();
			char* chars = (char*)reader.GetDataOfReadPosition();
			reader.SkipBytes(len);
			s.assign(chars, len);
			return true;
		}
	}
#endif

	Il2CppReflectionType* InterpreterImage::ReadSystemType(BlobReader& reader)
	{
		Il2CppString* fullName = ReadSerString(reader);
		if (!fullName)
		{
			return nullptr;
		}
		Il2CppReflectionType* type = ReadAttributeTypeName(fullName);
		if (!type)
		{
			std::string stdTypeName = il2cpp::utils::StringUtils::Utf16ToUtf8(fullName->chars);
			TEMP_FORMAT(errMsg, "CustomAttribute fixed arg type:System.Type fullName:'%s' not find", stdTypeName.c_str());
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeLoadException(errMsg));
		}
		return type;
	}

	Il2CppReflectionType* InterpreterImage::ReadAttributeTypeName(Il2CppString* name)
	{
		Il2CppReflectionType* reflection = GetReflectionTypeFromName(name);
		if (!_homologousTypeReferenceImage || !reflection)
		{
			return reflection;
		}
		return il2cpp::vm::Reflection::GetTypeObject(ResolveHomologousType(reflection->type));
	}

	const Il2CppType* InterpreterImage::ResolveHomologousType(const Il2CppType* type)
	{
		Il2CppType resolved = *type;
		switch (type->type)
		{
		case IL2CPP_TYPE_CLASS:
		case IL2CPP_TYPE_VALUETYPE:
		{
			const Il2CppTypeDefinition* definition =
				(const Il2CppTypeDefinition*)type->data.typeHandle;
			if (definition && DecodeImageIndex(definition->byvalTypeIndex) == _index)
			{
				resolved.data = GetIl2CppTypeFromRawTypeDefIndex(GetTypeRawIndex(definition))->data;
			}
			break;
		}
		case IL2CPP_TYPE_GENERICINST:
		{
			const Il2CppGenericClass* generic = type->data.generic_class;
			const Il2CppGenericInst* arguments = generic->context.class_inst;
			std::vector<const Il2CppType*> types(arguments->type_argc);
			for (uint32_t index = 0; index < arguments->type_argc; ++index)
			{
				types[index] = ResolveHomologousType(arguments->type_argv[index]);
			}
			resolved.data.generic_class = const_cast<Il2CppGenericClass*>(
				il2cpp::metadata::GenericMetadata::GetGenericClass(ResolveHomologousType(generic->type),
					il2cpp::vm::MetadataCache::GetGenericInst(types.data(), static_cast<uint32_t>(types.size()))));
			break;
		}
		case IL2CPP_TYPE_SZARRAY:
		case IL2CPP_TYPE_PTR:
			resolved.data.type = ResolveHomologousType(type->data.type);
			break;
		case IL2CPP_TYPE_ARRAY:
			resolved.data.array = const_cast<Il2CppArrayType*>(MetadataPool::GetPooledIl2CppArrayType(
				ResolveHomologousType(type->data.array->etype), type->data.array->rank));
			break;
		default:
			break;
		}
		return MetadataPool::GetPooledIl2CppType(resolved);
	}


	Il2CppObject* InterpreterImage::ReadBoxedValue(BlobReader& reader)
	{
		uint64_t obj = 0;
		Il2CppType kind = {};
		ReadCustomAttributeFieldOrPropType(reader, kind);
		ReadFixedArg(reader, &kind, &obj);
		Il2CppClass* valueType = il2cpp::vm::Class::FromIl2CppType(&kind);
		return il2cpp::vm::Object::Box(valueType, &obj);
	}

	void InterpreterImage::ReadFixedArg(BlobReader& reader, const Il2CppType* argType, void* data)
	{
		switch (argType->type)
		{
		case IL2CPP_TYPE_BOOLEAN:
		{
			*(byte*)data = reader.ReadByte();
			break;
		}
		case IL2CPP_TYPE_CHAR:
		{
			*(uint16_t*)data = reader.Read16();
			break;
		}
		case IL2CPP_TYPE_I1:
		case IL2CPP_TYPE_U1:
		{
			*(byte*)data = reader.ReadByte();
			break;
		}
		case IL2CPP_TYPE_I2:
		case IL2CPP_TYPE_U2:
		{
			*(uint16_t*)data = reader.Read16();
			break;
		}
		case IL2CPP_TYPE_I4:
		case IL2CPP_TYPE_U4:
		{
			*(uint32_t*)data = reader.Read32();
			break;
		}
		case IL2CPP_TYPE_I8:
		case IL2CPP_TYPE_U8:
		{
			*(uint64_t*)data = reader.Read64();
			break;
		}
		case IL2CPP_TYPE_R4:
		{
			*(float*)data = reader.ReadFloat();
			break;
		}
		case IL2CPP_TYPE_R8:
		{
			*(double*)data = reader.ReadDouble();
			break;
		}
		case IL2CPP_TYPE_SZARRAY:
		{
			uint32_t numElem = reader.Read32();
			if (numElem != (uint32_t)-1)
			{
				Il2CppClass* arrKlass = il2cpp::vm::Class::FromIl2CppType(argType);
				Il2CppArray* arr = il2cpp::vm::Array::New(il2cpp::vm::Class::GetElementClass(arrKlass), numElem);
				for (uint16_t i = 0; i < numElem; i++)
				{
					ReadFixedArg(reader, argType->data.type, GET_ARRAY_ELEMENT_ADDRESS(arr, i, arr->klass->element_size));
				}
				*(void**)data = arr;
			}
			else
			{
				*(void**)data = nullptr;
			}
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_STRING:
		{
			*(Il2CppString**)data = ReadSerString(reader);
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_OBJECT:
		{
			*(Il2CppObject**)data = ReadBoxedValue(reader);
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_CLASS:
		{
			Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(argType);
			if (!klass)
			{
				RaiseExecutionEngineException("type not find");
			}
			if (klass == il2cpp_defaults.object_class)
			{
				*(Il2CppObject**)data = ReadBoxedValue(reader);
			}
			else if (klass == il2cpp_defaults.systemtype_class)
			{
				*(Il2CppReflectionType**)data = ReadSystemType(reader);
			}
			else
			{
				TEMP_FORMAT(errMsg, "fixed arg type:%s.%s not support", klass->namespaze, klass->name);
				RaiseNotSupportedException(errMsg);
			}
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_VALUETYPE:
		{
			Il2CppClass* valueType = il2cpp::vm::Class::FromIl2CppType(argType);
			IL2CPP_ASSERT(valueType->enumtype);
			ReadFixedArg(reader, &valueType->element_class->byval_arg, data);
			break;
		}
		case IL2CPP_TYPE_SYSTEM_TYPE:
		{
			*(Il2CppReflectionType**)data = ReadSystemType(reader);
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_BOXED_OBJECT:
		{
			uint8_t fieldOrPropType = reader.ReadByte();
			IL2CPP_ASSERT(fieldOrPropType == 0x51);
			*(Il2CppObject**)data = ReadBoxedValue(reader);
			HYBRIDCLR_SET_WRITE_BARRIER((void**)data);
			break;
		}
		case IL2CPP_TYPE_ENUM:
		{
			Il2CppClass* valueType = il2cpp::vm::Class::FromIl2CppType(argType);
			IL2CPP_ASSERT(valueType->enumtype);
			ReadFixedArg(reader, &valueType->element_class->byval_arg, data);
			break;
		}
		default:
		{
			RaiseExecutionEngineException("not support fixed argument type");
		}
		}
	}

	void InterpreterImage::ReadCustomAttributeFieldOrPropType(BlobReader& reader, Il2CppType& type)
	{
		type.type = (Il2CppTypeEnum)reader.ReadByte();

		switch (type.type)
		{
		case IL2CPP_TYPE_BOOLEAN:
		case IL2CPP_TYPE_CHAR:
		case IL2CPP_TYPE_I1:
		case IL2CPP_TYPE_U1:
		case IL2CPP_TYPE_I2:
		case IL2CPP_TYPE_U2:
		case IL2CPP_TYPE_I4:
		case IL2CPP_TYPE_U4:
		case IL2CPP_TYPE_I8:
		case IL2CPP_TYPE_U8:
		case IL2CPP_TYPE_R4:
		case IL2CPP_TYPE_R8:
		case IL2CPP_TYPE_STRING:
		{
			break;
		}
		case IL2CPP_TYPE_SZARRAY:
		{
			Il2CppType eleType = {};
			ReadCustomAttributeFieldOrPropType(reader, eleType);
			type.data.type = MetadataPool::GetPooledIl2CppType(eleType);
			break;
		}
		case IL2CPP_TYPE_ENUM:
		{
			Il2CppString* enumTypeName = ReadSerString(reader);

			Il2CppReflectionType* enumType = ReadAttributeTypeName(enumTypeName);
			if (!enumType)
			{
				std::string stdStrName = il2cpp::utils::StringUtils::Utf16ToUtf8(enumTypeName->chars);
				TEMP_FORMAT(errMsg, "ReadCustomAttributeFieldOrPropType enum:'%s' not exists", stdStrName.c_str());
				RaiseExecutionEngineException(errMsg);
			}
			type = *enumType->type;
			break;
		}
		case IL2CPP_TYPE_SYSTEM_TYPE:
		{
			type = il2cpp_defaults.systemtype_class->byval_arg;
			break;
		}
		case IL2CPP_TYPE_BOXED_OBJECT:
		{
			type = il2cpp_defaults.object_class->byval_arg;
			break;
		}
		default:
		{
			TEMP_FORMAT(errMsg, "ReadCustomAttributeFieldOrPropType. image:%s unknown type:%d", GetIl2CppImage()->name, (int)type.type);
			RaiseBadImageException(errMsg);
		}
		}
	}

	void InterpreterImage::ReadMethodDefSig(BlobReader& reader, const Il2CppGenericContainer* klassGenericContainer, const Il2CppGenericContainer* methodGenericContainer, Il2CppMethodDefinition& methodDef, uint32_t parameterStart, uint32_t paramCapacity)
	{
		uint8_t rawSigFlags = reader.ReadByte();

		if (rawSigFlags & (uint8_t)MethodSigFlags::GENERIC)
		{
			//IL2CPP_ASSERT(false);
			uint32_t genParamCount = reader.ReadCompressedUint32();
			Il2CppGenericContainer* gc = GetGenericContainerByRawIndex(DecodeMetadataIndex(methodDef.genericContainerIndex));
			IL2CPP_ASSERT(gc->type_argc == genParamCount);
		}
		uint32_t paramCount = reader.ReadCompressedUint32();
		if (paramCount != paramCapacity)
		{
			RaiseBadImageException("method signature parameter count does not match metadata");
		}

		const Il2CppType* returnType = ReadType(reader, klassGenericContainer, methodGenericContainer);
		methodDef.returnType = AddIl2CppTypeCache(returnType);

		uint32_t readParamNum = 0;
		ParamDetail* contiguousParams = _params.GetOrCreateContiguous(parameterStart, paramCapacity);
		for (; reader.NonEmpty(); )
		{
			if (readParamNum >= paramCapacity)
			{
				RaiseBadImageException("method signature contains too many parameters");
			}
			ParamDetail& curParam = contiguousParams ? contiguousParams[readParamNum] : _params.GetOrCreate(parameterStart + readParamNum);
			curParam = {};
			const Il2CppType* type = ReadType(reader, klassGenericContainer, methodGenericContainer);
			curParam.parameterIndex = readParamNum;
			curParam.defaultValueIndex = kDefaultValueIndexNull;
			curParam.paramDef.typeIndex = AddIl2CppTypeCache(type);
			++readParamNum;
		}
		if (readParamNum != paramCount)
		{
			RaiseBadImageException("method signature contains too few parameters");
		}
	}

	const Il2CppType* InterpreterImage::GetModuleIl2CppType(uint32_t moduleRowIndex, uint32_t typeNamespace, uint32_t typeName, bool raiseExceptionIfNotFound)
	{
		IL2CPP_ASSERT(moduleRowIndex == 1);
		uint32_t encodedNamespaceIndex = EncodeWithIndex(typeNamespace);
		uint32_t encodedNameIndex = EncodeWithIndex(typeName);
		for (TypeDefinitionDetail& type : _typeDetails)
		{
			Il2CppTypeDefinition* typeDef = GetTypeDefinitionByTypeDetail(&type);
			if (typeDef->namespaceIndex == encodedNamespaceIndex && typeDef->nameIndex == encodedNameIndex)
			{
				return GetIl2CppTypeFromRawTypeDefIndex(GetTypeRawIndex(typeDef));
			}
		}
		if (!raiseExceptionIfNotFound)
		{
			return nullptr;
		}
		const char* typeNameStr = _rawImage->GetStringFromRawIndex(typeName);
		const char* typeNamespaceStr = _rawImage->GetStringFromRawIndex(typeNamespace);
		il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeLoadException(
			CStringToStringView(typeNamespaceStr),
			CStringToStringView(typeNameStr),
			CStringToStringView(_il2cppImage->nameNoExt)));
		return nullptr;
	}

	const Il2CppType* InterpreterImage::GetIl2CppTypeFromRawTypeDefIndex(uint32_t index)
	{
		return _homologousTypeReferenceImage
			? _homologousTypeReferenceImage->GetIl2CppTypeFromRawTypeDefIndex(index)
			: GetRawTypeDefinitionType(index);
	}
}
}
