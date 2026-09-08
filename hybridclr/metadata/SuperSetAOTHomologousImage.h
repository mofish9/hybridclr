#pragma once

#include "AOTHomologousImage.h"
#include "utils/Il2CppHashMap.h"
#include "utils/HashUtils.h"
#include <unordered_map>
#include <unordered_set>

namespace hybridclr
{
	namespace metadata
	{

		struct SuperSetTypeIntermediateInfo
		{
			bool inited;
			//uint32_t homoRowIndex;
			uint32_t homoParentRowIndex;
			uint32_t homoMethodStartIndex; // start from 1
			uint32_t homoFieldStartIndex; // start from 1
			//const char* name;
			//const char* namespaze;
			//int32_t aotTypeIndex; // il2cpp type index
			const Il2CppType* aotIl2CppType;
			const Il2CppTypeDefinition* aotTypeDef;
			//const Il2CppClass* aotKlass;
		};

		struct SuperSetTypeDefDetail
		{
			//bool inited;
			//uint32_t homoRowIndex;
			//uint32_t homoParentRowIndex;
			//uint32_t homoMethodStartIndex; // start from 1
			//uint32_t homoFieldStartIndex; // start from 1
			//const char* name;
			//const char* namespaze;
			//int32_t aotTypeIndex; // il2cpp type index
			const Il2CppType* aotIl2CppType;
			//const Il2CppTypeDefinition* aotTypeDef;
			//const Il2CppClass* aotKlass;
		};

		struct SuperSetMethodDefDetail
		{
			//uint32_t homoRowIndex; 
			//MethodRefSig signature;
			//const Il2CppTypeDefinition* declaringTypeDef;
			//const Il2CppClass* declaringKlass;
			//const char* name;
			const Il2CppMethodDefinition* aotMethodDef;
			bool interpreterFallback;
		};

		struct SuperSetFieldDefDetail
		{
			//uint32_t homoRowIndex;
			//const char* name;
			//Il2CppType type;
			//const Il2CppTypeDefinition* declaringTypeDef;
			const Il2CppType* declaringIl2CppType;
			const Il2CppFieldDefinition* aotFieldDef;
			bool interpreterFallback;
		};

		class InterpreterImage;

		class SuperSetAOTHomologousImage : public AOTHomologousImage
		{
		public:
			SuperSetAOTHomologousImage() : AOTHomologousImage() {}

			void SetInterpreterFallbackImage(InterpreterImage* image, bool isDheImage = false)
			{
				_interpreterFallbackImage = image;
				_isDheImage = isDheImage;
			}

			void InitRuntimeMetadatas() override;
			void InitTypeReferences();

			const Il2CppType* ReadTypeFromResolutionScope(uint32_t scope, uint32_t typeNamespace, uint32_t typeName) override;
			MethodBody* GetMethodBody(uint32_t token) override;
			const Il2CppType* GetIl2CppTypeFromRawTypeDefIndex(uint32_t index) override;
			Il2CppGenericContainer* GetGenericContainerByRawIndex(uint32_t index) override;
			Il2CppGenericContainer* GetGenericContainerByTypeDefRawIndex(int32_t typeDefIndex) override;
			const Il2CppMethodDefinition* GetMethodDefinitionFromRawIndex(uint32_t index) override;
			void ReadFieldRefInfoFromFieldDefToken(uint32_t rowIndex, FieldRefInfo& ret) override;
			Il2CppClass* FindSupplementalType(const char* namespaze, const char* name) override;
			void GetSupplementalTypes(std::vector<const Il2CppClass*>& types) override;
			Il2CppClass* GetFirstSupplementalNestedType(Il2CppClass* klass, void** iter) override;
			bool TryGetNextSupplementalNestedType(Il2CppClass* klass, void** iter,
				Il2CppClass** nestedType) override;
			const MethodInfo* GetFirstSupplementalMethod(Il2CppClass* klass, void** iter) override;
			bool TryGetNextSupplementalMethod(Il2CppClass* klass, void** iter,
				const MethodInfo** method) override;
			Image* GetSupplementalMethodImage(const MethodInfo* method) override;
			Image* GetMethodResolveImage(const MethodInfo* method) override;
			size_t GetSupplementalMethodCount(Il2CppClass* klass) override;
			const MethodInfo* ResolveLogicalMethod(const MethodInfo* method) override;
			const Il2CppType* GetDheCurrentType(const Il2CppType* type) override;
			bool TryGetDheCurrentInterfaceMethod(const Il2CppClass* klass,
				uint16_t logicalSlot, const MethodInfo*& method) override;
			FieldInfo* GetFirstSupplementalField(Il2CppClass* klass, void** iter) override;
			bool TryGetNextSupplementalField(Il2CppClass* klass, void** iter,
				FieldInfo** field) override;
			size_t GetSupplementalFieldCount(Il2CppClass* klass) override;
			bool IsRemovedField(const FieldInfo* field) override;
			const FieldInfo* ResolveSupplementalField(const FieldInfo* field) override;
			const Il2CppFieldDefinition* ResolveSupplementalFieldDefinition(
				const Il2CppType* type, const char* name, const Il2CppType* fieldType) override;
			Il2CppClass* GetSupplementalFieldLogicalParent(const FieldInfo* field) override;
			bool TryGetCustomAttributeSource(uint32_t token,
				const Il2CppImage*& sourceImage, uint32_t& sourceToken) override;
			bool HasLogicalPropertyView(Il2CppClass* klass) override;
			const PropertyInfo* GetFirstLogicalProperty(Il2CppClass* klass, void** iter) override;
			bool TryGetNextLogicalProperty(Il2CppClass* klass, void** iter,
				const PropertyInfo** property) override;
			size_t GetLogicalPropertyCount(Il2CppClass* klass) override;
			bool HasLogicalEventView(Il2CppClass* klass) override;
			const EventInfo* GetFirstLogicalEvent(Il2CppClass* klass, void** iter) override;
			bool TryGetNextLogicalEvent(Il2CppClass* klass, void** iter,
				const EventInfo** eventInfo) override;
			size_t GetLogicalEventCount(Il2CppClass* klass) override;
		private:

			void InitTypes0(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos);
			void InitNestedClass(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos);
			void InitType(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos, SuperSetTypeIntermediateInfo& type);
			void InitTypes1(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos);
			void InitSupplementalTypes(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos);
			void ReadMethodDefSig(BlobReader& reader, MethodRefSig& method);
			void InitMethods(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos);
			void InitFields(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos);
			void InitPropertiesAndEvents(
				std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos);
			const MethodInfo* GetLogicalMethod(const MethodInfo* currentMethod);
			const std::vector<const MethodInfo*>* GetSupplementalMethods(Il2CppClass* klass);
			const std::vector<FieldInfo*>* GetSupplementalFields(Il2CppClass* klass);

			const Il2CppType* _defaultIl2CppType;

			std::vector<SuperSetTypeDefDetail> _typeDefs;
			std::vector<SuperSetTypeIntermediateInfo> _typeIntermediateInfos;
			Il2CppHashMap<int32_t, SuperSetTypeDefDetail*, il2cpp::utils::PassThroughHash<int32_t>> _aotTypeIndex2TypeDefs;

			Il2CppHashMap<uint32_t, SuperSetMethodDefDetail*, il2cpp::utils::PassThroughHash<uint32_t>> _token2MethodDefs;
			std::vector<SuperSetMethodDefDetail> _methodDefs;

			std::vector<SuperSetFieldDefDetail> _fields;
			InterpreterImage* _interpreterFallbackImage = nullptr;
			bool _isDheImage = false;
			std::vector<Il2CppClass*> _supplementalTypes;
			std::unordered_map<Il2CppClass*, std::vector<Il2CppClass*>> _supplementalNestedTypes;
			std::unordered_map<Il2CppClass*, std::vector<const MethodInfo*>> _supplementalMethods;
			std::unordered_map<Il2CppClass*, std::vector<const MethodInfo*>> _genericSupplementalMethods;
			std::unordered_map<const MethodInfo*, Image*> _supplementalMethodImages;
			std::unordered_map<const MethodInfo*, const MethodInfo*> _logicalMethods;
			std::unordered_map<const Il2CppClass*, std::vector<const MethodInfo*>> _interfaceMethods;
			std::unordered_map<Il2CppClass*, std::vector<FieldInfo*>> _supplementalFields;
			std::unordered_map<const FieldInfo*, FieldInfo*> _logicalFields;
			// All accesses use g_MetadataLock; published vectors never change.
			std::unordered_map<Il2CppClass*, std::vector<FieldInfo*>> _genericSupplementalFields;
			std::unordered_map<const FieldInfo*, Il2CppClass*> _supplementalFieldLogicalParents;
			std::unordered_set<uint32_t> _matchedAotFieldTokens;
			std::unordered_set<const FieldInfo*> _removedFields;
			std::unordered_map<uint32_t, uint32_t> _customAttributeTokens;
			std::unordered_map<Il2CppClass*, std::vector<const PropertyInfo*>> _logicalProperties;
			std::unordered_map<Il2CppClass*, std::vector<const EventInfo*>> _logicalEvents;
		};
	}
}
