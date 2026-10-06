// Traditional SUPERSET implementation retained from pre-DHE f40c6f08ccd0391ad9285276b4cc21ada3a180ab.
#include "../AotModeConfig.h"
#if HYBRIDCLR_ENABLE_AOT_SELECTION
#pragma once

#include "AOTHomologousImage.h"
#include "utils/Il2CppHashMap.h"
#include "utils/HashUtils.h"

namespace hybridclr
{
	namespace metadata
	{

		struct TraditionalTypeIntermediateInfo
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

		struct TraditionalTypeDefDetail
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

		struct TraditionalMethodDefDetail
		{
			//uint32_t homoRowIndex;
			//MethodRefSig signature;
			//const Il2CppTypeDefinition* declaringTypeDef;
			//const Il2CppClass* declaringKlass;
			//const char* name;
			const Il2CppMethodDefinition* aotMethodDef;
		};

		struct TraditionalFieldDefDetail
		{
			//uint32_t homoRowIndex;
			//const char* name;
			//Il2CppType type;
			//const Il2CppTypeDefinition* declaringTypeDef;
			const Il2CppType* declaringIl2CppType;
			const Il2CppFieldDefinition* aotFieldDef;
		};

		class TraditionalAOTHomologousImage : public AOTHomologousImage
		{
		public:
			TraditionalAOTHomologousImage() : AOTHomologousImage() {}

			void InitRuntimeMetadatas() override;

			const Il2CppType* ReadTypeFromResolutionScope(uint32_t scope, uint32_t typeNamespace, uint32_t typeName) override;
			MethodBody* GetMethodBody(uint32_t token) override;
			const Il2CppType* GetIl2CppTypeFromRawTypeDefIndex(uint32_t index) override;
			Il2CppGenericContainer* GetGenericContainerByRawIndex(uint32_t index) override;
			Il2CppGenericContainer* GetGenericContainerByTypeDefRawIndex(int32_t typeDefIndex) override;
			const Il2CppMethodDefinition* GetMethodDefinitionFromRawIndex(uint32_t index) override;
			void ReadFieldRefInfoFromFieldDefToken(uint32_t rowIndex, FieldRefInfo& ret) override;
		private:

			void InitTypes0(std::vector<TraditionalTypeIntermediateInfo>& typeIntermediateInfos);
			void InitNestedClass(std::vector<TraditionalTypeIntermediateInfo>& typeIntermediateInfos);
			void InitType(std::vector<TraditionalTypeIntermediateInfo>& typeIntermediateInfos, TraditionalTypeIntermediateInfo& type);
			void InitTypes1(std::vector<TraditionalTypeIntermediateInfo>& typeIntermediateInfos);
			void ReadMethodDefSig(BlobReader& reader, MethodRefSig& method);
			void InitMethods(std::vector<TraditionalTypeIntermediateInfo>& typeIntermediateInfos);
			void InitFields(std::vector<TraditionalTypeIntermediateInfo>& typeIntermediateInfos);

			const Il2CppType* _defaultIl2CppType;

			std::vector<TraditionalTypeDefDetail> _typeDefs;
			Il2CppHashMap<int32_t, TraditionalTypeDefDetail*, il2cpp::utils::PassThroughHash<int32_t>> _aotTypeIndex2TypeDefs;

			Il2CppHashMap<uint32_t, TraditionalMethodDefDetail*, il2cpp::utils::PassThroughHash<uint32_t>> _token2MethodDefs;
			std::vector<TraditionalMethodDefDetail> _methodDefs;

			std::vector<TraditionalFieldDefDetail> _fields;
		};
	}
}
#endif
