#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "os/Atomic.h"

#if HYBRIDCLR_UNITY_2021_OR_NEW
#include "metadata/CustomAttributeDataReader.h"
#include "CustomAttributeDataWriter.h"
#endif

#include "Image.h"
#include "CustomAttributeDataWriter.h"

namespace hybridclr
{
namespace metadata
{
	void FlushMetadataProfile();

	class ClassFieldLayoutCalculator;
	class SuperSetAOTHomologousImage;
	struct InterfaceOffsetInfo
	{
		const Il2CppType* type;
		uint32_t offset;
	};


	struct TypeDefinitionDetail
	{
		uint32_t methodImplStart;
		uint32_t methodImplCount;
		uint16_t virtualMethodCount;
		uint32_t vtableCount;
		Il2CppTypeDefinitionSizes typeSizes;
		VirtualMethodImpl* vtable;
	};

	struct ParamDetail
	{
		Il2CppParameterDefinition paramDef;
		uint32_t parameterIndex;
		uint32_t defaultValueIndex; // -1 for invalid
	};

	class ParamDetailStorage
	{
	public:
		void Reset(uint32_t count)
		{
			_count = count;
			_chunks.clear();
			_chunks.resize((count + kChunkMask) >> kChunkBits);
		}

		ParamDetail& GetOrCreate(uint32_t index)
		{
			IL2CPP_ASSERT(index < _count);
			std::unique_ptr<ParamDetail[]>& chunk = _chunks[index >> kChunkBits];
			if (!chunk)
			{
				chunk.reset(new ParamDetail[kChunkSize]);
			}
			return chunk[index & kChunkMask];
		}

		ParamDetail* GetOrCreateContiguous(uint32_t index, uint32_t count)
		{
			IL2CPP_ASSERT(index <= _count && count <= _count - index);
			if (count == 0)
			{
				return nullptr;
			}
			uint32_t itemIndex = index & kChunkMask;
			if (count > kChunkSize - itemIndex)
			{
				return nullptr;
			}
			std::unique_ptr<ParamDetail[]>& chunk = _chunks[index >> kChunkBits];
			if (!chunk)
			{
				chunk.reset(new ParamDetail[kChunkSize]);
			}
			return chunk.get() + itemIndex;
		}

		ParamDetail& operator[](uint32_t index)
		{
			IL2CPP_ASSERT(index < _count && _chunks[index >> kChunkBits]);
			return _chunks[index >> kChunkBits][index & kChunkMask];
		}

		const ParamDetail& operator[](uint32_t index) const
		{
			IL2CPP_ASSERT(index < _count && _chunks[index >> kChunkBits]);
			return _chunks[index >> kChunkBits][index & kChunkMask];
		}

	private:
		static const uint32_t kChunkBits = 10;
		static const uint32_t kChunkSize = 1u << kChunkBits;
		static const uint32_t kChunkMask = kChunkSize - 1;

		uint32_t _count = 0;
		std::vector<std::unique_ptr<ParamDetail[]>> _chunks;
	};

	struct FieldDetail
	{
		Il2CppFieldDefinition fieldDef;
		uint32_t typeDefIndex;
		uint32_t offset;
		uint32_t defaultValueIndex; // -1 for invalid
	};

	struct PropertyDetail
	{
		const char* name;
		uint16_t flags;
		uint32_t signatureBlobIndex;
		uint32_t getterMethodIndex; // start from 1;
		uint32_t setterMethodIndex;
		const Il2CppTypeDefinition* declaringType;
		Il2CppPropertyDefinition il2cppDefinition;
	};

	struct EventDetail
	{
		const char* name;
		uint16_t eventFlags;
		uint32_t eventType; // TypeDefOrRef codedIndex
		uint32_t addMethodIndex; // start from 1
		uint32_t removeMethodIndex; // start from 1
		uint32_t fireMethodIndex; // start from 1;
#if HYBRIDCLR_UNITY_2019
		const Il2CppTypeDefinition* declaringType;
		Il2CppEventDefinition il2cppDefinition;
#endif
	};

	struct CustomAttribute
	{
		uint32_t ctorMethodToken;
		uint32_t value;
	};

	struct CustomAttributesInfo
	{
		int32_t typeRangeIndex;
		int32_t inited;
		void* dataStartPtr;
		void* dataEndPtr;
	};

	class CustomAttributeTokenMap
	{
	public:
		void Reset(uint32_t expectedCount)
		{
			_size = 0;
			if (expectedCount == 0)
			{
				_entries.clear();
				_mask = 0;
				return;
			}
			IL2CPP_ASSERT(expectedCount < (1u << 30));
			uint32_t capacity = 4;
			while (capacity < expectedCount * 2)
			{
				capacity <<= 1;
			}
			_entries.assign(capacity, Entry{});
			_mask = capacity - 1;
		}

		bool TryGet(uint32_t token, uint32_t& handleIndex) const
		{
			if (_entries.empty())
			{
				return false;
			}
			uint32_t slot = Hash(token) & _mask;
			for (;;)
			{
				const Entry& entry = _entries[slot];
				if (entry.token == token)
				{
					handleIndex = entry.handleIndex;
					return true;
				}
				if (entry.token == 0)
				{
					return false;
				}
				slot = (slot + 1) & _mask;
			}
		}

		void Insert(uint32_t token, uint32_t handleIndex)
		{
			IL2CPP_ASSERT(token != 0 && !_entries.empty() && _size * 2 < _entries.size());
			uint32_t slot = Hash(token) & _mask;
			while (_entries[slot].token != 0)
			{
				IL2CPP_ASSERT(_entries[slot].token != token);
				slot = (slot + 1) & _mask;
			}
			_entries[slot] = { token, handleIndex };
			++_size;
		}

		uint32_t Size() const
		{
			return _size;
		}

	private:
		struct Entry
		{
			uint32_t token;
			uint32_t handleIndex;
		};

		static uint32_t Hash(uint32_t value)
		{
			value ^= value >> 16;
			value *= 0x7feb352d;
			value ^= value >> 15;
			value *= 0x846ca68b;
			return value ^ (value >> 16);
		}

		std::vector<Entry> _entries;
		uint32_t _size = 0;
		uint32_t _mask = 0;
	};

#if HYBRIDCLR_UNITY_2021_OR_NEW
	struct CustomAttributeCtorInfo
	{
		const MethodInfo* method;
		MethodIndex methodIndex;
		uint16_t parameterCount;
	};
#endif

	struct MethodMetadataDetail
	{
		uint32_t signatureBlobIndex;
		uint32_t rawParameterStart;
		uint32_t rawParameterCount;
		int32_t initialized;
	};

	class Il2CppTypeCacheStorage
	{
	public:
		Il2CppTypeCacheStorage() : _size(0), _levels{}
		{
		}

		~Il2CppTypeCacheStorage()
		{
			for (uint32_t levelIndex = 0; levelIndex < kLevelSize; ++levelIndex)
			{
				Level* level = _levels[levelIndex];
				if (!level)
				{
					continue;
				}
				for (uint32_t chunkIndex = 0; chunkIndex < kLevelSize; ++chunkIndex)
				{
					delete[] level->chunks[chunkIndex];
				}
				delete level;
			}
		}

		uint32_t size() const
		{
			return _size;
		}

		void push_back(const Il2CppType* type)
		{
			IL2CPP_ASSERT(_size < (1u << 30));
			uint32_t levelIndex = _size >> (kLevelBits * 2);
			uint32_t chunkIndex = (_size >> kLevelBits) & kLevelMask;
			uint32_t itemIndex = _size & kLevelMask;
			Level*& level = _levels[levelIndex];
			if (!level)
			{
				level = new Level();
			}
			const Il2CppType**& chunk = level->chunks[chunkIndex];
			if (!chunk)
			{
				chunk = new const Il2CppType*[kLevelSize]();
			}
			chunk[itemIndex] = type;
			++_size;
		}

		const Il2CppType* operator[](uint32_t index) const
		{
			const Level* level = _levels[index >> (kLevelBits * 2)];
			IL2CPP_ASSERT(level);
			const Il2CppType* const* chunk = level->chunks[(index >> kLevelBits) & kLevelMask];
			IL2CPP_ASSERT(chunk && chunk[index & kLevelMask]);
			return chunk[index & kLevelMask];
		}

	private:
		static const uint32_t kLevelBits = 10;
		static const uint32_t kLevelSize = 1u << kLevelBits;
		static const uint32_t kLevelMask = kLevelSize - 1;

		struct Level
		{
			Level() : chunks{}
			{
			}

			const Il2CppType** chunks[kLevelSize];
		};

		Il2CppTypeCacheStorage(const Il2CppTypeCacheStorage&) = delete;
		Il2CppTypeCacheStorage& operator=(const Il2CppTypeCacheStorage&) = delete;

		uint32_t _size;
		Level* _levels[kLevelSize];
	};


#if HYBRIDCLR_UNITY_2021_OR_NEW
	enum class BlobSource
	{
		RAW_IMAGE = 0,
		CONVERTED_IL2CPP_FORMAT = 1,
	};
#endif

	struct ImplMapInfo
	{
		const char* moduleName;
		const char* importName;
		uint32_t mappingFlags;
	};

	class InterpreterImage : public Image
	{
	public:

		static void Initialize();

		static uint32_t AllocImageIndex(uint32_t dllLength);

		static void RegisterImage(InterpreterImage* image);

		static InterpreterImage* GetImage(uint32_t imageIndex)
		{
			IL2CPP_ASSERT(imageIndex < kMaxMetadataImageCount);
			return il2cpp::os::Atomic::LoadPointerAcquire(&s_images[imageIndex]);
		}

	private:

		static InterpreterImage* s_images[kMaxMetadataImageCount];

	public:

		InterpreterImage(uint32_t imageIndex) : _inited(false), _il2cppImage(nullptr), _index(imageIndex), _customAttributesInitialized(0), _threadStaticFieldsInitialized(false), _customAttributeRangeCount(0), _propertyEventMetadataInitialized(0)
			, _methodMetadataInitializedCount(0), _paramCount(0), _classLayoutCalculator(nullptr), _classLayoutInitializedTypeCount(0), _classLayoutInitializableTypeCount(0)
			, _vtableInitializedTypeCount(0), _vtableInitializableTypeCount(0)
#if HYBRIDCLR_UNITY_2021_OR_NEW
			, _constValues(1024), _il2cppFormatCustomDataBlob(256), _tempCtorArgBlob(256), _tempFieldBlob(256), _tempPropertyBlob(256)
#endif
		{

		}

		LoadImageErrorCode Load(const void* imageData, size_t length)
		{
			if (_inited)
			{
				RaiseExecutionEngineException("image can't be inited again");
			}
			_inited = true;
			LoadImageErrorCode err = InitRawImage(imageData, length);
			if (err != LoadImageErrorCode::OK)
			{
				return err;
			}
			err = _rawImage->Load(imageData, length);
			if (err != LoadImageErrorCode::OK)
			{
				delete _rawImage;
				_rawImage = nullptr;
				return err;
			}
			return LoadImageErrorCode::OK;
		}

		bool IsInitialized() const
		{
			return _inited;
		}

		uint32_t GetIndex() const
		{
			return _index;
		}

		void FlushClassLayoutCacheInstrumentation();

		const Il2CppImage* GetIl2CppImage() const
		{
			return _il2cppImage;
		}

		uint32_t EncodeWithIndex(uint32_t rawIndex) const
		{
			return EncodeImageAndMetadataIndex(_index, rawIndex);
		}

		uint32_t EncodeWithIndexExcept0(uint32_t rawIndex) const
		{
			return rawIndex != 0 ? EncodeImageAndMetadataIndex(_index, rawIndex) : 0;
		}

		MethodBody* GetMethodBody(uint32_t token) override
		{
			IL2CPP_ASSERT(DecodeTokenTableType(token) == TableType::METHOD);
			uint32_t rowIndex = DecodeTokenRowIndex(token);
			IL2CPP_ASSERT(rowIndex > 0 && rowIndex <= (uint32_t)_methodDefines.size());


			EnsureMethodMetadataInitialized(rowIndex - 1);
			const Il2CppMethodDefinition* methodDef = &_methodDefines[rowIndex - 1];
			bool isGenericMethod = methodDef->genericContainerIndex != kGenericContainerIndexInvalid || _typesDefines[DecodeMetadataIndex(methodDef->declaringType)].genericContainerIndex != kGenericContainerIndexInvalid;

			TbMethod methodData = _rawImage->ReadMethod(rowIndex);
			MethodBody* resultMethodBody = new (HYBRIDCLR_MALLOC_ZERO(sizeof(MethodBody))) MethodBody();
			ReadMethodBody(*methodDef, methodData, *resultMethodBody);
			return resultMethodBody;
		}

		// type index start from 0, difference with table index...
		Il2CppMetadataTypeHandle GetAssemblyTypeHandleFromRawIndex(AssemblyTypeIndex index) const
		{
			IL2CPP_ASSERT(DecodeImageIndex(index) == 0);
			IL2CPP_ASSERT(index >= 0 && (size_t)index < _typesDefines.size());
			return (Il2CppMetadataTypeHandle)&_typesDefines[index];
		}

		Il2CppMetadataTypeHandle GetAssemblyExportedTypeHandleFromRawIndex(AssemblyTypeIndex index) const
		{
			IL2CPP_ASSERT(DecodeImageIndex(index) == 0);
			IL2CPP_ASSERT(index >= 0 && (size_t)index < _typesDefines.size());
			return (Il2CppMetadataTypeHandle)&_exportedTypeDefines[index];
		}

		const Il2CppTypeDefinitionSizes* GetTypeDefinitionSizesFromRawIndex(TypeDefinitionIndex index)
		{
			IL2CPP_ASSERT((size_t)index < _typeDetails.size());
			return &_typeDetails[index].typeSizes;
		}

		const char* GetStringFromRawIndex(StringIndex index) const
		{
			IL2CPP_ASSERT(DecodeImageIndex(index) == 0);
			return _rawImage->GetStringFromRawIndex(index);
		}

		uint32_t GetTypeRawIndex(const Il2CppTypeDefinition* typeDef) const
		{
			return (uint32_t)(typeDef - &_typesDefines[0]);
		}

		uint32_t GetTypeDefinitionCount() const
		{
			return (uint32_t)_typesDefines.size();
		}

		Il2CppTypeDefinition* GetTypeDefinitionByTypeDetail(const TypeDefinitionDetail* typeDetail)
		{
			uint32_t index = (uint32_t)(typeDetail - &_typeDetails[0]);
			return &_typesDefines[index];
		}

		uint32_t GetTypeRawIndexByEncodedIl2CppTypeIndex(int32_t il2cppTypeIndex) const
		{
			return GetTypeRawIndex((const Il2CppTypeDefinition*)GetIl2CppTypeFromRawIndex(DecodeMetadataIndex(il2cppTypeIndex))->data.typeHandle);
		}

		const Il2CppTypeDefinition* GetTypeFromRawIndex(uint32_t index) const
		{
			IL2CPP_ASSERT((size_t)index < _typesDefines.size());
			return &_typesDefines[index];
		}

		const Il2CppType* GetIl2CppTypeFromRawIndex(uint32_t index) const;

		const Il2CppType* GetIl2CppTypeFromRawTypeDefIndex(uint32_t index) override;
		const Il2CppType* ReadTypeFromResolutionScope(uint32_t scope, uint32_t typeNamespace, uint32_t typeName) override;

		const Il2CppType* GetRawTypeDefinitionType(uint32_t index) const
		{
			IL2CPP_ASSERT(index < (uint32_t)_typesDefines.size());
			return GetIl2CppTypeFromRawIndex(DecodeMetadataIndex(_typesDefines[index].byvalTypeIndex));
		}

		const Il2CppFieldDefinition* GetFieldDefinitionFromRawIndex(uint32_t index)
		{
			IL2CPP_ASSERT(index < (uint32_t)_fieldDetails.size());
			EnsureFieldMetadataInitialized(index);
			return &(_fieldDetails[index].fieldDef);
		}

		const Il2CppFieldDefinition* GetFieldDefinitionFromRawIndexLocked(uint32_t index)
		{
			IL2CPP_ASSERT(index < (uint32_t)_fieldDetails.size());
			EnsureFieldMetadataInitializedLocked(index);
			return &(_fieldDetails[index].fieldDef);
		}

		void EnsureFieldMetadataInitialized(uint32_t index);
		void EnsureFieldMetadataInitializedLocked(uint32_t index);
		void EnsureTypeFieldMetadataInitializedLocked(const Il2CppTypeDefinition* typeDef);

		const FieldDetail& GetFieldDetailFromRawIndex(uint32_t index)
		{
			IL2CPP_ASSERT(index < (uint32_t)_fieldDetails.size());
			return _fieldDetails[index];
		}

		const Il2CppMethodDefinition* GetMethodDefinitionFromRawIndex(uint32_t index) override
		{
			IL2CPP_ASSERT((size_t)index < _methodDefines.size());
			EnsureMethodMetadataInitialized(index);
			return &_methodDefines[index];
		}

		const Il2CppMethodDefinition* GetMethodDefinitionFromRawIndexLocked(uint32_t index)
		{
			IL2CPP_ASSERT((size_t)index < _methodDefines.size());
			EnsureMethodMetadataInitializedLocked(index);
			return &_methodDefines[index];
		}

		const Il2CppMethodDefinition* GetMethodDefinitionHeaderFromRawIndex(uint32_t index) const
		{
			IL2CPP_ASSERT((size_t)index < _methodDefines.size());
			return &_methodDefines[index];
		}

		void EnsureTypeMethodMetadataInitializedLocked(const Il2CppTypeDefinition* typeDef);
		void EnsureVTableInitializedLocked(const Il2CppTypeDefinition* typeDef);

		bool HasMethodImpls(const Il2CppTypeDefinition* typeDef) const
		{
			uint32_t index = (uint32_t)(typeDef - &_typesDefines[0]);
			IL2CPP_ASSERT(index < (uint32_t)_typeDetails.size());
			return _typeDetails[index].methodImplCount != 0;
		}

		uint16_t GetVirtualMethodCount(const Il2CppTypeDefinition* typeDef) const
		{
			uint32_t index = (uint32_t)(typeDef - &_typesDefines[0]);
			IL2CPP_ASSERT(index < (uint32_t)_typeDetails.size());
			return _typeDetails[index].virtualMethodCount;
		}

		VTableSetUp* GetVTableTree(const Il2CppTypeDefinition* typeDef) const
		{
			if (_vtableTreesByTypeDefinition.empty())
			{
				return nullptr;
			}
			uint32_t index = (uint32_t)(typeDef - &_typesDefines[0]);
			IL2CPP_ASSERT(index < _vtableTreesByTypeDefinition.size());
			return _vtableTreesByTypeDefinition[index];
		}

		bool OwnsVTableTreeCache(const Il2CppType2TypeDeclaringTreeMap* cache) const
		{
			return cache == &_cacheTrees;
		}

		bool HasDirectVTableTreeCache() const
		{
			return !_vtableTreesByTypeDefinition.empty();
		}

		const VirtualMethodImpl* GetPublishedVTable(const Il2CppTypeDefinition* typeDef, uint32_t& count) const
		{
			uint32_t index = (uint32_t)(typeDef - &_typesDefines[0]);
			IL2CPP_ASSERT(index < _typeDetails.size() && typeDef->interfaceOffsetsStart != 0);
			const TypeDefinitionDetail& detail = _typeDetails[index];
			IL2CPP_ASSERT(detail.vtableCount == typeDef->vtable_count);
			count = detail.vtableCount;
			return detail.vtable;
		}

		void SetVTableTree(const Il2CppTypeDefinition* typeDef, VTableSetUp* tree)
		{
			uint32_t index = (uint32_t)(typeDef - &_typesDefines[0]);
			IL2CPP_ASSERT(index < _vtableTreesByTypeDefinition.size() && !_vtableTreesByTypeDefinition[index]);
			_vtableTreesByTypeDefinition[index] = tree;
		}

		MethodIndex GetMethodIndexFromDefinition(const Il2CppMethodDefinition* methodDefine)
		{
			return EncodeWithIndex((uint32_t)(methodDefine - &_methodDefines[0]));
		}

		const Il2CppGenericParameter* GetGenericParameterByGlobalIndex(uint32_t index)
		{
			IL2CPP_ASSERT(index < (uint32_t)_genericParams.size());
			return &_genericParams[index];
		}

		const Il2CppGenericParameter* GetGenericParameterByRawIndex(const Il2CppGenericContainer* container, uint32_t index)
		{
			uint32_t globalIndex = DecodeMetadataIndex(container->genericParameterStart) + index;
			IL2CPP_ASSERT(globalIndex < (uint32_t)_genericParams.size());
			return &_genericParams[globalIndex];
		}

		Il2CppGenericContainer* GetGenericContainerByRawIndex(uint32_t index) override
		{
			if (index != kGenericContainerIndexInvalid)
			{
				IL2CPP_ASSERT(index < (uint32_t)_genericContainers.size());
				return &_genericContainers[index];
			}
			return nullptr;
		}

		Il2CppGenericContainer* GetGenericContainerByTypeDefinition(const Il2CppTypeDefinition* typeDef)
		{
			GenericContainerIndex idx = DecodeMetadataIndex(typeDef->genericContainerIndex);
			if (idx != kGenericContainerIndexInvalid)
			{
				IL2CPP_ASSERT(idx < (GenericContainerIndex)_genericContainers.size());
				return &_genericContainers[idx];
			}
			return nullptr;
		}

		Il2CppGenericContainer* GetGenericContainerByTypeDefRawIndex(int32_t typeDefIndex) override
		{
			IL2CPP_ASSERT(typeDefIndex < (int32_t)_typeDetails.size());
			return GetGenericContainerByTypeDefinition(&_typesDefines[typeDefIndex]);
		}

		const il2cpp::utils::dynamic_array<MethodImpl> GetTypeMethodImplByTypeDefinition(const Il2CppTypeDefinition* typeDef);

		const Il2CppType* GetGenericParameterConstraintFromIndex(GenericParameterConstraintIndex index);

		Il2CppClass* GetNestedTypeFromOffset(const Il2CppClass* klass, TypeNestedTypeIndex offset);
		Il2CppClass* GetNestedTypeFromOffset(const Il2CppTypeDefinition* typeDef, TypeNestedTypeIndex offset);

		const MethodInfo* GetMethodInfoFromMethodDefinitionRawIndex(uint32_t index);
		const MethodInfo* GetMethodInfoFromMethodDefinition(const Il2CppMethodDefinition* methodDef);
		const Il2CppMethodDefinition* GetMethodDefinitionFromVTableSlot(const Il2CppTypeDefinition* typeDefine, int32_t vTableSlot);
		const MethodInfo* GetMethodInfoFromVTableSlot(const Il2CppClass* klass, int32_t vTableSlot);

		Il2CppTypeDefinition* GetNestedTypes(Il2CppTypeDefinition* handle, void** iter);

		void GetClassAndMethodGenericContainerFromGenericContainerIndex(GenericContainerIndex idx, const Il2CppGenericContainer*& klassGc, const Il2CppGenericContainer*& methodGc);

		Il2CppMethodPointer GetAdjustorThunk(uint32_t token);
		Il2CppMethodPointer GetMethodPointer(uint32_t token);
		InvokerMethod GetMethodInvoker(uint32_t token);

		const Il2CppParameterDefinition* GetParameterDefinitionFromIndex(uint32_t index)
		{
			IL2CPP_ASSERT(index < _paramCount);
			return &_params[index].paramDef;
		}

		const Il2CppParameterDefaultValue* GetParameterDefaultValueEntryByRawIndex(uint32_t index)
		{
			IL2CPP_ASSERT(index < _paramCount);
			uint32_t defaultValueIndex = _params[index].defaultValueIndex;
			return defaultValueIndex != kDefaultValueIndexNull ? &_paramDefaultValues[defaultValueIndex] : nullptr;
		}

		uint32_t GetFieldOffset(const Il2CppTypeDefinition* typeDef, int32_t fieldIndexInType)
		{
			uint32_t fieldActualIndex = DecodeMetadataIndex(typeDef->fieldStart) + fieldIndexInType;
			IL2CPP_ASSERT(fieldActualIndex < (uint32_t)_fieldDetails.size());
			return _fieldDetails[fieldActualIndex].offset;
		}

		uint32_t GetFieldOffset(TypeDefinitionIndex typeIndex, int32_t fieldIndexInType)
		{
			Il2CppTypeDefinition* typeDef = &_typesDefines[typeIndex];
			return GetFieldOffset(typeDef, fieldIndexInType);
		}

		uint32_t GetFieldOffset(const Il2CppClass* klass, int32_t fieldIndexInType)
		{
			Il2CppTypeDefinition* typeDef = (Il2CppTypeDefinition*)(klass->typeMetadataHandle);
			return GetFieldOffset(typeDef, fieldIndexInType);
		}

		int32_t GetPackingSize(const Il2CppTypeDefinition* typeDef) const
		{
			int32_t typeIndex = GetTypeRawIndex(typeDef);
			auto it = _classLayouts.find(typeIndex);
			return it != _classLayouts.end() ? it->second.packingSize : 0;
		}

		TbClassLayout GetClassLayout(const Il2CppTypeDefinition* typeDef) const
		{
			int32_t typeIndex = GetTypeRawIndex(typeDef);
			auto it = _classLayouts.find(typeIndex);
			return it != _classLayouts.end() ? it->second : TbClassLayout{};
		}

		const Il2CppFieldDefaultValue* GetFieldDefaultValueEntryByRawIndex(uint32_t index)
		{
			IL2CPP_ASSERT(index < (uint32_t)_fieldDetails.size());
			EnsureFieldMetadataInitialized(index);
			uint32_t fdvIndex = _fieldDetails[index].defaultValueIndex;
			IL2CPP_ASSERT(fdvIndex != kDefaultValueIndexNull);
			return &_fieldDefaultValues[fdvIndex];
		}

#if HYBRIDCLR_UNITY_2021_OR_NEW
		static uint32_t EncodeWithBlobSource(uint32_t index, BlobSource source)
		{
			return (index << 1) | (uint32_t)source;
		}
#endif

		const uint8_t* GetFieldOrParameterDefalutValueByRawIndex(uint32_t index)
		{
#if !HYBRIDCLR_UNITY_2021_OR_NEW
			return _rawImage->GetFieldOrParameterDefalutValueByRawIndex(index);
#else
			BlobSource source = (BlobSource)(index & 0x1);
			uint32_t offset = index >> 1;
			if (source == BlobSource::RAW_IMAGE)
			{
				return _rawImage->GetFieldOrParameterDefalutValueByRawIndex(offset);
			}
			else
			{
				return _constValues.DataAt(offset);
			}
#endif
		}

#if HYBRIDCLR_UNITY_2021_OR_NEW
		DefaultValueDataIndex ConvertConstValue(CustomAttributeDataWriter& writer, uint32_t blobIndex, const Il2CppType* type);
#endif

		Il2CppPropertyDefinition* GetPropertyDefinitionFromIndex(PropertyIndex index)
		{
			EnsurePropertyEventMetadataInitialized();
			IL2CPP_ASSERT(index > 0 && index <= (int32_t)_propeties.size());
			PropertyDetail& pd = _propeties[(uint32_t)index - 1];
			return &pd.il2cppDefinition;
		}

		Il2CppMetadataPropertyInfo GetPropertyInfo(const Il2CppClass* klass, TypePropertyIndex index)
		{
			EnsurePropertyEventMetadataInitialized();
			const Il2CppTypeDefinition* typeDef = (Il2CppTypeDefinition*)klass->typeMetadataHandle;
			IL2CPP_ASSERT(typeDef->propertyStart);
			uint32_t rowIndex = DecodeMetadataIndex(typeDef->propertyStart) + index;
			PropertyDetail& pd = _propeties[rowIndex - 1];
			uint32_t baseMethodIdx = DecodeMetadataIndex(typeDef->methodStart) + 1;
#if UNITY_ENGINE_TUANJIE
			const MethodInfo* getter = pd.getterMethodIndex ? il2cpp::vm::Class::GetOrSetupOneMethod(const_cast<Il2CppClass*>(klass), pd.getterMethodIndex - baseMethodIdx) : nullptr;
			const MethodInfo* setter = pd.setterMethodIndex ? il2cpp::vm::Class::GetOrSetupOneMethod(const_cast<Il2CppClass*>(klass), pd.setterMethodIndex - baseMethodIdx) : nullptr;
#else
			const MethodInfo* getter = pd.getterMethodIndex ? klass->methods[pd.getterMethodIndex - baseMethodIdx] : nullptr;
			const MethodInfo* setter = pd.setterMethodIndex ? klass->methods[pd.setterMethodIndex - baseMethodIdx] : nullptr;
#endif
			return { pd.name, getter, setter, pd.flags, EncodeToken(TableType::PROPERTY, rowIndex) };
		}

#ifdef HYBRIDCLR_UNITY_2019
		const Il2CppEventDefinition* GetEventDefinitionFromIndex(EventIndex index)
		{
			EnsurePropertyEventMetadataInitialized();
			IL2CPP_ASSERT(index > 0 && index <= (int32_t)_events.size());
			EventDetail& pd = _events[index - 1];
			return &pd.il2cppDefinition;
		}
#endif


		Il2CppMetadataEventInfo GetEventInfo(const Il2CppClass* klass, TypeEventIndex index)
		{
			EnsurePropertyEventMetadataInitialized();
			const Il2CppTypeDefinition* typeDef = (Il2CppTypeDefinition*)klass->typeMetadataHandle;
			IL2CPP_ASSERT(typeDef->eventStart);
			uint32_t rowIndex = DecodeMetadataIndex(typeDef->eventStart) + index;
			EventDetail& pd = _events[rowIndex - 1];
			uint32_t baseMethodIdx = DecodeMetadataIndex(typeDef->methodStart) + 1;
#if UNITY_ENGINE_TUANJIE
			const MethodInfo* addOn = pd.addMethodIndex ? il2cpp::vm::Class::GetOrSetupOneMethod(const_cast<Il2CppClass*>(klass), pd.addMethodIndex - baseMethodIdx) : nullptr;
			const MethodInfo* removeOn = pd.removeMethodIndex ? il2cpp::vm::Class::GetOrSetupOneMethod(const_cast<Il2CppClass*>(klass), pd.removeMethodIndex - baseMethodIdx) : nullptr;
			const MethodInfo* raiseOn = pd.fireMethodIndex ? il2cpp::vm::Class::GetOrSetupOneMethod(const_cast<Il2CppClass*>(klass), pd.fireMethodIndex - baseMethodIdx) : nullptr;
#else
			const MethodInfo* addOn = pd.addMethodIndex ? klass->methods[pd.addMethodIndex - baseMethodIdx] : nullptr;
			const MethodInfo* removeOn = pd.removeMethodIndex ? klass->methods[pd.removeMethodIndex - baseMethodIdx] : nullptr;
			const MethodInfo* raiseOn = pd.fireMethodIndex ? klass->methods[pd.fireMethodIndex - baseMethodIdx] : nullptr;
#endif
			const Il2CppType* eventType = ReadTypeFromToken(
				GetGenericContainerByTypeDefinition(typeDef), nullptr,
				DecodeTypeDefOrRefOrSpecCodedIndexTableType(pd.eventType),
				DecodeTypeDefOrRefOrSpecCodedIndexRowIndex(pd.eventType));
			return { pd.name, eventType, addOn, removeOn, raiseOn,
				EncodeToken(TableType::EVENT, rowIndex) };
		}

		const Il2CppAssembly* GetReferencedAssembly(int32_t referencedAssemblyTableIndex, const Il2CppAssembly assembliesTable[], int assembliesCount);

		Il2CppMetadataCustomAttributeHandle GetCustomAttributeTypeToken(uint32_t token)
		{
			EnsureCustomAttributesInitialized();
			uint32_t handleIndex;
			return _tokenCustomAttributes.TryGet(token, handleIndex)
				? (Il2CppMetadataCustomAttributeHandle)&_customAttributeHandles[handleIndex] : nullptr;
		}

		CustomAttributeIndex GetCustomAttributeIndex(uint32_t token)
		{
			EnsureCustomAttributesInitialized();
			uint32_t handleIndex;
			return _tokenCustomAttributes.TryGet(token, handleIndex)
				? _customAttributeInfos[handleIndex].typeRangeIndex : kCustomAttributeIndexInvalid;
		}

#if !HYBRIDCLR_UNITY_2021_OR_NEW
		std::tuple<void*, void*> GetCustomAttributeDataRange(uint32_t token)
		{
			EnsureCustomAttributesInitialized();
			const Il2CppCustomAttributeTypeRange* dataRangeCur = (const Il2CppCustomAttributeTypeRange*)GetCustomAttributeTypeToken(token);
			CustomAttributeIndex curIndex = DecodeMetadataIndex(GET_CUSTOM_ATTRIBUTE_TYPE_RANGE_START(*dataRangeCur));
			CustomAttributeIndex nextIndex = DecodeMetadataIndex(GET_CUSTOM_ATTRIBUTE_TYPE_RANGE_START(*(dataRangeCur + 1)));
			CustomAttribute& curCa = _customAttribues[curIndex];
			CustomAttribute& nextCa = _customAttribues[nextIndex];
			return std::tuple<void*, void*>((void*)_rawImage->GetBlobReaderByRawIndex(curCa.value).GetData(), (void*)_rawImage->GetBlobReaderByRawIndex(nextCa.value).GetData());
		}

		CustomAttributesCache* GenerateCustomAttributesCacheInternal(const Il2CppCustomAttributeTypeRange* typeRange)
		{
			CustomAttributeIndex index = (CustomAttributeIndex)(typeRange - (const Il2CppCustomAttributeTypeRange*)&_customAttributeHandles[0]);
			IL2CPP_ASSERT(index >= 0 && index < (CustomAttributeIndex)_customAttributeHandles.size());
			return GenerateCustomAttributesCacheInternal(index);
		}

		bool HasAttribute(CustomAttributeIndex index, Il2CppClass* attribute)
		{
			const Il2CppCustomAttributeTypeRange* typeRange = &_customAttributeHandles[DecodeMetadataIndex(index)];
			return HasAttribute(typeRange, attribute);
		}

		bool HasAttribute(const Il2CppCustomAttributeTypeRange* typeRange, Il2CppClass* attribute)
		{
			CustomAttributesCache* attrCache = GenerateCustomAttributesCacheInternal(typeRange);
			return HasAttribute(attrCache, attribute);
		}

		bool HasAttributeByToken(uint32_t token, Il2CppClass* attribute)
		{
			CustomAttributeIndex index = GetCustomAttributeIndex(token);
			if (index == kCustomAttributeIndexInvalid)
			{
				return false;
			}
			CustomAttributesCache* attrCache = GenerateCustomAttributesCacheInternal(DecodeMetadataIndex(index));
			return HasAttribute(attrCache, attribute);
		}

		bool HasAttribute(CustomAttributesCache* attrCache, Il2CppClass* attribute)
		{
			for (int i = 0; i < attrCache->count; i++)
			{
				Il2CppObject* attrObj = attrCache->attributes[i];
				if (il2cpp::vm::Class::IsAssignableFrom(attribute, attrObj->klass))
				{
					return true;
				}
			}
			return false;
		}

		CustomAttributesCache* GenerateCustomAttributesCacheInternal(CustomAttributeIndex index);
#else

		void InitCustomAttributeData(CustomAttributesInfo& cai, const Il2CppCustomAttributeTypeRange& dataRange);
		const CustomAttributeCtorInfo& GetOrCreateCustomAttributeCtorInfo(uint32_t ctorMethodToken);
			
		il2cpp::metadata::CustomAttributeDataReader CreateCustomAttributeDataReader(Il2CppMetadataCustomAttributeHandle handle)
		{
			EnsureCustomAttributesInitialized();
			const Il2CppCustomAttributeTypeRange* dataRange = (const Il2CppCustomAttributeTypeRange*)handle;
			uint32_t handleIndex = (uint32_t)(dataRange - _customAttributeHandles.data());
			IL2CPP_ASSERT(handleIndex < _customAttributeInfos.size() && _customAttributeHandles[handleIndex].token == dataRange->token);
			CustomAttributesInfo& cai = _customAttributeInfos[handleIndex];
			if (!IsMetadataPublished(&cai.inited))
			{
				InitCustomAttributeData(cai, *dataRange);
			}
#if HYBRIDCLR_UNITY_2022_OR_NEW
			return il2cpp::metadata::CustomAttributeDataReader(_il2cppImage, cai.dataStartPtr, cai.dataEndPtr);
#else
			return il2cpp::metadata::CustomAttributeDataReader(cai.dataStartPtr, cai.dataEndPtr);
#endif
		}

		std::tuple<void*, void*> CreateCustomAttributeDataTuple(const Il2CppCustomAttributeDataRange* dataRange)
		{
			EnsureCustomAttributesInitialized();
			const Il2CppCustomAttributeTypeRange* typeRange = (const Il2CppCustomAttributeTypeRange*)dataRange;
			uint32_t handleIndex = (uint32_t)(typeRange - _customAttributeHandles.data());
			IL2CPP_ASSERT(handleIndex < _customAttributeInfos.size() && _customAttributeHandles[handleIndex].token == dataRange->token);
			CustomAttributesInfo& cai = _customAttributeInfos[handleIndex];
			if (!IsMetadataPublished(&cai.inited))
			{
				InitCustomAttributeData(cai, *dataRange);
			}
			return std::tuple<void*, void*>(cai.dataStartPtr, cai.dataEndPtr);
		}

		std::tuple<void*, void*> CreateCustomAttributeDataTupleByToken(uint32_t token)
		{
			const Il2CppCustomAttributeTypeRange* dataRangeCur = (const Il2CppCustomAttributeTypeRange*)GetCustomAttributeTypeToken(token);
			return dataRangeCur ? CreateCustomAttributeDataTuple(dataRangeCur) : std::tuple<void*, void*>(nullptr, nullptr);
		}

#if !HYBRIDCLR_UNITY_2022_OR_NEW
		CustomAttributesCache* GenerateCustomAttributesCacheInternal(const Il2CppCustomAttributeTypeRange* typeRange)
		{
			CustomAttributeIndex index = (CustomAttributeIndex)(typeRange - (const Il2CppCustomAttributeTypeRange*)&_customAttributeHandles[0]);
			IL2CPP_ASSERT(index >= 0 && index < (CustomAttributeIndex)_customAttributeHandles.size());
			return GenerateCustomAttributesCacheInternal(index);
		}

		CustomAttributesCache* GenerateCustomAttributesCacheInternal(CustomAttributeIndex index);
#endif

		void BuildCustomAttributesData(CustomAttributesInfo& cai, const Il2CppCustomAttributeTypeRange& typeRange);
		void ConvertILCustomAttributeData2Il2CppFormat(const MethodInfo* ctorMethod, BlobReader& reader);
		void ConvertFixedArg(CustomAttributeDataWriter& writer, BlobReader& reader, const Il2CppType* type, bool writeType);
		void ConvertBoxedValue(CustomAttributeDataWriter& writer, BlobReader& reader, bool writeType);
		void ConvertSystemType(CustomAttributeDataWriter& writer, BlobReader& reader, bool writeType);
		void WriteEncodeTypeEnum(CustomAttributeDataWriter& writer, const Il2CppType* type);
		void GetFieldDeclaringTypeIndexAndFieldIndexByName(const Il2CppTypeDefinition* declaringType, const char* name, int32_t& typeIndex, int32_t& fieldIndex);
		void GetPropertyDeclaringTypeIndexAndPropertyIndexByName(const Il2CppTypeDefinition* declaringType, const char* name, int32_t& typeIndex, int32_t& fieldIndex);
#endif

		ImplMapInfo* GetImplMapInfo(uint32_t token)
		{
			auto it = _implMapInfos.find(token);
			return it != _implMapInfos.end() ? &it->second : nullptr;
		}

		Il2CppClass* GetTypeInfoFromTypeDefinitionRawIndex(uint32_t index);

		const Il2CppType* GetInterfaceFromGlobalOffset(TypeInterfaceIndex offset);
		const Il2CppType* GetInterfaceFromIndex(const Il2CppClass* klass, TypeInterfaceIndex index);
		const Il2CppType* GetInterfaceFromOffset(const Il2CppClass* klass, TypeInterfaceIndex offset);
		const Il2CppType* GetInterfaceFromOffset(const Il2CppTypeDefinition* typeDefine, TypeInterfaceIndex offset);

		Il2CppInterfaceOffsetInfo GetInterfaceOffsetInfo(const Il2CppTypeDefinition* typeDefine, TypeInterfaceOffsetIndex index);

		uint32_t AddIl2CppTypeCache(const Il2CppType* type);
		void FreezeIl2CppTypeCache();

		uint32_t AddIl2CppGenericContainers(Il2CppGenericContainer& geneContainer);

		const Il2CppType* GetModuleIl2CppType(uint32_t moduleRowIndex, uint32_t typeNamespace, uint32_t typeName, bool raiseExceptionIfNotFound) override;
		void ReadFieldRefInfoFromFieldDefToken(uint32_t rowIndex, FieldRefInfo& ret) override;
		void ReadMethodDefSig(BlobReader& reader, const Il2CppGenericContainer* klassGenericContainer, const Il2CppGenericContainer* methodGenericContainer, Il2CppMethodDefinition& methodDef, uint32_t parameterStart, uint32_t paramCapacity);

		void InitBasic(Il2CppImage* image);
		void BuildIl2CppImage(Il2CppImage* image);
		void BuildIl2CppAssembly(Il2CppAssembly* assembly);

		void InitRuntimeMetadatas() override;
		void SetHomologousTypeReferenceImage(SuperSetAOTHomologousImage* image)
		{
			_homologousTypeReferenceImage = image;
		}
		bool TryApplyClassLayoutLocked(Il2CppClass* klass);
	protected:

		void InitTypeDefs_0();
		void InitTypeDefs_1();
		void InitTypeDefs_2();
		void InitConsts();

		void InitClass();

		void InitParamDefs();
		void InitGenericParamConstraintDefs();
		void InitGenericParamDefs0();
		void InitGenericParamDefs();
		void InitFieldDefs();
		void InitFieldLayouts();
		void InitFieldRVAs();
		void InitMethodDefs0();
		void InitMethodDefs();
		void EnsureMethodMetadataInitialized(uint32_t index);
		void EnsureMethodMetadataInitializedLocked(uint32_t index);
		void BuildMethodMetadata(uint32_t index);
		void BuildFieldMetadata(uint32_t index);
		void InitMethodImpls0();
		void InitNestedClass();
		void InitClassLayouts0();
		void InitClassLayouts();
		void InitClassLayoutsLazy();
		void InitClassLayout(uint32_t index);
		void InitCustomAttributes();
		void InitThreadStaticFields();
		void EnsureThreadStaticFieldsInitializedLocked();
		void EnsureCustomAttributesInitialized();
		void BuildCustomAttributeIndexes();
		void InitModuleRefs();
		void InitImplMaps();
		void InitProperties();
		void InitEvents();
		void EnsurePropertyEventMetadataInitialized();
		void BuildProperties();
		void BuildEvents();
		void InitMethodSemantics();
		void InitInterfaces();
		void InitVTables();

		void ComputeHasFinalizer(Il2CppTypeDefinition *def, std::vector<bool> &computFlags);
		void InitHasFinalizers();
		void ComputeVTable(TypeDefinitionDetail* tdd);

		void SetIl2CppImage(Il2CppImage* image)
		{
			_il2cppImage = image;
		}

		Il2CppString* ReadSerString(BlobReader& reader);
#if HYBRIDCLR_UNITY_2021_OR_NEW
		bool ReadUTF8SerString(BlobReader& reader, std::string& s);
#endif
		Il2CppReflectionType* ReadSystemType(BlobReader& reader);
		Il2CppReflectionType* ReadAttributeTypeName(Il2CppString* name);
		const Il2CppType* ResolveHomologousType(const Il2CppType* type);
		Il2CppObject* ReadBoxedValue(BlobReader& reader);
		void ReadFixedArg(BlobReader& reader, const Il2CppType* argType, void* data);
		void ReadCustomAttributeFieldOrPropType(BlobReader& reader, Il2CppType& type);
#if !HYBRIDCLR_UNITY_2021_OR_NEW
		void ConstructCustomAttribute(BlobReader& reader, Il2CppObject* obj, const MethodInfo* ctorMethod);
#endif


		bool _inited;
		Il2CppImage* _il2cppImage;
		SuperSetAOTHomologousImage* _homologousTypeReferenceImage = nullptr;
		const uint32_t _index;
		static bool IsMetadataPublished(const int32_t* initialized)
		{
			return Baselib_atomic_load_32_acquire(initialized) != 0;
		}

		static void PublishMetadata(int32_t* initialized)
		{
			Baselib_atomic_store_32_release(initialized, 1);
		}

		static bool IsFieldMetadataInitialized(const FieldDetail& field)
		{
			uint32_t typeIndex = static_cast<uint32_t>(Baselib_atomic_load_32_acquire(
				reinterpret_cast<const int32_t*>(&field.fieldDef.typeIndex)));
			return typeIndex != kTypeIndexInvalid;
		}

		static void PublishFieldMetadata(FieldDetail& field, uint32_t typeIndex)
		{
			Baselib_atomic_store_32_release(reinterpret_cast<int32_t*>(&field.fieldDef.typeIndex),
				static_cast<int32_t>(typeIndex));
		}

		int32_t _customAttributesInitialized;
		bool _threadStaticFieldsInitialized;
		uint32_t _customAttributeRangeCount;
		int32_t _propertyEventMetadataInitialized;

		std::vector<TypeDefinitionDetail> _typeDetails;
		std::vector<Il2CppTypeDefinition> _typesDefines;
		std::vector<Il2CppTypeDefinition> _exportedTypeDefines;

		Il2CppTypeCacheStorage _types;
		Il2CppHashMap<const Il2CppType*, uint32_t, Il2CppTypeHashShallow, Il2CppTypeEqualityComparerShallow> _type2Indexs;
		struct FrozenTypeIndex
		{
			const Il2CppType* type;
			uint32_t index;
			uint32_t hash;
		};
		std::vector<FrozenTypeIndex> _frozenTypeIndexes;
		std::vector<TypeIndex> _interfaceDefines;
		std::vector<InterfaceOffsetInfo> _interfaceOffsets;

		std::vector<Il2CppMethodDefinition> _methodDefines;
		std::unique_ptr<MethodMetadataDetail[]> _methodMetadataDetails;
		uint32_t _methodMetadataInitializedCount;

		ParamDetailStorage _params;
		uint32_t _paramCount;
		std::vector<Il2CppParameterDefaultValue> _paramDefaultValues;
		std::unordered_map<uint32_t, uint32_t> _rawParamDefaultValueIndexes;

		std::vector<Il2CppGenericParameter> _genericParams;
		std::vector<TypeIndex> _genericConstraints; // raw TypeIndex
		std::vector<Il2CppGenericContainer> _genericContainers;

		std::vector<FieldDetail> _fieldDetails;
		std::vector<Il2CppFieldDefaultValue> _fieldDefaultValues;

		std::unordered_map<uint32_t, TbClassLayout> _classLayouts;
		std::vector<uint32_t> _nestedTypeDefineIndexs;

		// runtime data 
		std::vector<Il2CppClass*> _classList;
		ClassFieldLayoutCalculator* _classLayoutCalculator;
		std::vector<uint8_t> _classLayoutInitialized;
		uint32_t _classLayoutInitializedTypeCount;
		uint32_t _classLayoutInitializableTypeCount;
		uint32_t _vtableInitializedTypeCount;
		uint32_t _vtableInitializableTypeCount;
		std::vector<VTableSetUp*> _vtableTreesByTypeDefinition;
		Il2CppType2TypeDeclaringTreeMap _cacheTrees;
#if HYBRIDCLR_UNITY_2021_OR_NEW
		CustomAttributeDataWriter _constValues;
#endif


		CustomAttributeTokenMap _tokenCustomAttributes;
		std::vector<CustomAttributesInfo> _customAttributeInfos;
		std::vector<Il2CppCustomAttributeTypeRange> _customAttributeHandles;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
		std::vector<CustomAttributesCache*> _customAttribtesCaches;
#endif
#if HYBRIDCLR_UNITY_2021_OR_NEW
		CustomAttributeDataWriter _il2cppFormatCustomDataBlob;
		CustomAttributeDataWriter _tempCtorArgBlob;
		CustomAttributeDataWriter _tempFieldBlob;
		CustomAttributeDataWriter _tempPropertyBlob;
		std::unordered_map<uint32_t, CustomAttributeCtorInfo> _customAttributeCtorInfos;
#endif
		std::vector<CustomAttribute> _customAttribues;

		std::vector<PropertyDetail> _propeties;
		std::vector<EventDetail> _events;

		std::vector<const char*> _moduleRefs;
		std::unordered_map<uint32_t, ImplMapInfo> _implMapInfos;
	};
}
}
