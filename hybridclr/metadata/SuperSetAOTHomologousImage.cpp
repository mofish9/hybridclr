#include "SuperSetAOTHomologousImage.h"

#include "vm/MetadataLock.h"
#include "vm/GlobalMetadata.h"
#include "vm/Class.h"
#include "vm/Image.h"
#include "vm/Exception.h"
#include "vm/MetadataCache.h"
#include "metadata/GenericMetadata.h"
#include "MetadataPool.h"
#include "InterpreterImage.h"
#include "MetadataModule.h"

namespace hybridclr
{
namespace metadata
{



	const Il2CppMethodDefinition* FindMatchMethod(const Il2CppTypeDefinition* aotTypeDef, const SuperSetMethodDefDetail& method2, const char* methodName, const MethodRefSig& methodSignature)
	{
		const Il2CppGenericContainer* klassGenContainer = aotTypeDef->genericContainerIndex != kGenericContainerIndexInvalid ?
			(const Il2CppGenericContainer*)il2cpp::vm::GlobalMetadata::GetGenericContainerFromIndex(aotTypeDef->genericContainerIndex) : nullptr;
		for (uint16_t i = 0; i < aotTypeDef->method_count; i++)
		{
			//const MethodInfo* method1 = klass1->methods[i];
			const Il2CppMethodDefinition* aotMethodDef = il2cpp::vm::GlobalMetadata::GetMethodDefinitionFromIndex(aotTypeDef->methodStart + i);
			const char* aotMethodName = il2cpp::vm::GlobalMetadata::GetStringFromIndex(aotMethodDef->nameIndex);
			if (std::strcmp(aotMethodName, methodName))
			{
				continue;
			}
			if (IsMatchMethodSig(aotMethodDef, methodSignature, klassGenContainer))
			{
				return aotMethodDef;
			}
		}
		return nullptr;
	}


	const Il2CppFieldDefinition* FindMatchField(const Il2CppTypeDefinition* aotTypeDef, const SuperSetFieldDefDetail& field2, const char* fieldName, const Il2CppType* fieldType)
	{
		const Il2CppGenericContainer* klassGenContainer = aotTypeDef->genericContainerIndex != kGenericContainerIndexInvalid ?
			(const Il2CppGenericContainer*)il2cpp::vm::GlobalMetadata::GetGenericContainerFromIndex(aotTypeDef->genericContainerIndex) : nullptr;
		for (uint16_t i = 0; i < aotTypeDef->field_count; i++)
		{
			//const FieldInfo* field1 = klass1->fields + i;
			const Il2CppFieldDefinition* aotField = il2cpp::vm::GlobalMetadata::GetFieldDefinitionFromTypeDefAndFieldIndex(aotTypeDef, i);
			const char* aotFieldName = il2cpp::vm::GlobalMetadata::GetStringFromIndex(aotField->nameIndex);
			if (std::strcmp(aotFieldName, fieldName))
			{
				continue;
			}
			const Il2CppType* aotFieldType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(aotField->typeIndex);
			if (IsMatchSigType(aotFieldType, fieldType, klassGenContainer, nullptr))
			{
				return aotField;
			}
		}
		return nullptr;
	}

	const MethodInfo* FindRuntimeMethod(const Il2CppType* declaringType,
		const Il2CppMethodDefinition* methodDefinition)
	{
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(declaringType);
		il2cpp::vm::Class::SetupMethods(klass);
		for (uint16_t index = 0; index < klass->method_count; ++index)
		{
			const MethodInfo* method = klass->methods[index];
			if (method && !method->is_inflated &&
				(const Il2CppMethodDefinition*)method->methodMetadataHandle == methodDefinition)
			{
				return method;
			}
		}
		return nullptr;
	}

	void SuperSetAOTHomologousImage::InitRuntimeMetadatas()
	{
		_defaultIl2CppType = &il2cpp_defaults.missing_class->byval_arg;

		std::vector< SuperSetTypeIntermediateInfo> typeIntermediateInfos;
		InitTypes0(typeIntermediateInfos);
		InitNestedClass(typeIntermediateInfos);
		InitTypes1(typeIntermediateInfos);

		InitMethods(typeIntermediateInfos);
		InitFields(typeIntermediateInfos);
		InitPropertiesAndEvents(typeIntermediateInfos);
	}

	void SuperSetAOTHomologousImage::InitTypes0(std::vector< SuperSetTypeIntermediateInfo>& typeIntermediateInfos)
	{
		const Table& typeDefTb = _rawImage->GetTable(TableType::TYPEDEF);
		uint32_t typeCount = typeDefTb.rowNum;
		typeIntermediateInfos.resize(typeCount);
		_typeDefs.resize(typeCount);
		_aotTypeIndex2TypeDefs.resize(typeCount);
	}

	void SuperSetAOTHomologousImage::InitNestedClass(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos)
	{
		const Table& nestedClassTb = _rawImage->GetTable(TableType::NESTEDCLASS);
		for (uint32_t i = 0; i < nestedClassTb.rowNum; i++)
		{
			TbNestedClass data = _rawImage->ReadNestedClass(i + 1);
			SuperSetTypeIntermediateInfo& nestedType = typeIntermediateInfos[data.nestedClass - 1];
			nestedType.homoParentRowIndex = data.enclosingClass;
		}
	}

	void SuperSetAOTHomologousImage::InitType(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos, SuperSetTypeIntermediateInfo& type)
	{
		if (type.inited)
		{
			return;
		}
		type.inited = true;
		uint32_t rowIndex = (uint32_t)(&type - &typeIntermediateInfos[0] + 1);
		TbTypeDef data = _rawImage->ReadTypeDef(rowIndex);

		type.homoMethodStartIndex = data.methodList;
		type.homoFieldStartIndex = data.fieldList;

		const char* name = _rawImage->GetStringFromRawIndex(data.typeName);
		const char* namespaze = _rawImage->GetStringFromRawIndex(data.typeNamespace);
		if (type.homoParentRowIndex)
		{
			SuperSetTypeIntermediateInfo& parent = typeIntermediateInfos[type.homoParentRowIndex - 1];
			InitType(typeIntermediateInfos, parent);
			const Il2CppTypeDefinition* parentTypeDef = parent.aotTypeDef;
			if (parentTypeDef == nullptr)
			{
				goto labelInitDefault;
			}

			void* iter = nullptr;
			for (const Il2CppTypeDefinition* nextTypeDef; (nextTypeDef = (const Il2CppTypeDefinition*)il2cpp::vm::GlobalMetadata::GetNestedTypes((Il2CppMetadataTypeHandle)parentTypeDef, &iter));)
			{
				const char* nestedTypeName = il2cpp::vm::GlobalMetadata::GetStringFromIndex(nextTypeDef->nameIndex);
				IL2CPP_ASSERT(nestedTypeName);
				if (!std::strcmp(name, nestedTypeName))
				{
					type.aotTypeDef = nextTypeDef;
					//type.aotTypeIndex = nextTypeDef->byvalTypeIndex;
					type.aotIl2CppType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(nextTypeDef->byvalTypeIndex);
					//type.aotKlass = il2cpp::vm::GlobalMetadata::GetTypeInfoFromHandle((Il2CppMetadataTypeHandle)nextTypeDef);
					return;
				}
			}
		}
		else
		{
			const Il2CppTypeDefinition* aotTypeDef = (const Il2CppTypeDefinition*)il2cpp::vm::Image::TypeHandleFromName(_targetAssembly->image, namespaze, name);
			if (aotTypeDef)
			{
				type.aotTypeDef = aotTypeDef;
				type.aotIl2CppType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(aotTypeDef->byvalTypeIndex);
				//type.aotTypeIndex = type.aotTypeDef->byvalTypeIndex;
				return;
			}
		}
		labelInitDefault:
		if (_interpreterFallbackImage)
		{
			const uint32_t rawTypeIndex = rowIndex - 1;
			type.aotIl2CppType = _interpreterFallbackImage->GetIl2CppTypeFromRawTypeDefIndex(rawTypeIndex);
		}
		else
		{
			type.aotIl2CppType = _defaultIl2CppType;
		}
		//TEMP_FORMAT(msg, "type: %s::%s can't find homologous type in assembly:%s", type.namespaze, type.name, _targetAssembly->aname.name);
		//RaiseExecutionEngineException(msg);
	}

	void SuperSetAOTHomologousImage::InitTypes1(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos)
	{
		for (SuperSetTypeIntermediateInfo& td : typeIntermediateInfos)
		{
			//uint32_t rowIndex = ++index;
			//TbTypeDef data = _rawImage->ReadTypeDef(rowIndex);

			//td.inited = false;
			//td.homoParentRowIndex = 0;
			//td.homoRowIndex = rowIndex;
			//td.homoMethodStartIndex = data.methodList;
			//td.homoFieldStartIndex = data.fieldList;

			//td.name = _rawImage->GetStringFromRawIndex(data.typeName);
			//td.namespaze = _rawImage->GetStringFromRawIndex(data.typeNamespace);
			InitType(typeIntermediateInfos, td);
		}

		uint32_t index = 0;
		for (SuperSetTypeIntermediateInfo& td : typeIntermediateInfos)
		{
			const uint32_t rawTypeIndex = index;
			SuperSetTypeDefDetail& type = _typeDefs[index++];
			type.aotIl2CppType = td.aotIl2CppType;
			if (td.aotTypeDef)
			{
				_aotTypeIndex2TypeDefs[il2cpp::vm::GlobalMetadata::GetIndexForTypeDefinition(td.aotTypeDef)] = &type;
				_customAttributeTokens[td.aotTypeDef->token] =
					EncodeToken(TableType::TYPEDEF, rawTypeIndex + 1);
			}
			else if (_interpreterFallbackImage && rawTypeIndex != 0)
			{
				Il2CppClass* supplemental =
					_interpreterFallbackImage->GetTypeInfoFromTypeDefinitionRawIndex(rawTypeIndex);
				_supplementalTypes.push_back(supplemental);
				if (td.homoParentRowIndex != 0)
				{
					SuperSetTypeIntermediateInfo& parent =
						typeIntermediateInfos[td.homoParentRowIndex - 1];
					if (parent.aotTypeDef && parent.aotIl2CppType)
					{
						Il2CppClass* parentClass =
							il2cpp::vm::Class::FromIl2CppType(parent.aotIl2CppType);
						supplemental->declaringType = parentClass;
						_supplementalNestedTypes[parentClass].push_back(supplemental);
					}
				}
			}
		}
	}

	void SuperSetAOTHomologousImage::InitMethods(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos)
	{
		const Table& methodTb = _rawImage->GetTable(TableType::METHOD);
		uint32_t methodCount = methodTb.rowNum;
		_methodDefs.resize(methodCount);
		//_token2MethodDefs.resize(methodCount * 2);
		uint32_t typeCount = (uint32_t)typeIntermediateInfos.size();
		for (SuperSetTypeIntermediateInfo& type : typeIntermediateInfos)
		{
			uint32_t nextTypeIndex = (uint32_t)(&type - &typeIntermediateInfos[0] + 1);
			uint32_t nextTypeMethodStartIndex = nextTypeIndex < typeCount ? typeIntermediateInfos[nextTypeIndex].homoMethodStartIndex : methodCount + 1;


			for (uint32_t i = type.homoMethodStartIndex; i < nextTypeMethodStartIndex ; i++)
			{
				SuperSetMethodDefDetail& method = _methodDefs[i - 1];
				TbMethod data = _rawImage->ReadMethod(i);
				const MethodInfo* currentMethod = _interpreterFallbackImage
					? _interpreterFallbackImage->GetMethodInfoFromMethodDefinitionRawIndex(i - 1)
					: nullptr;
				const MethodInfo* logicalMethod = nullptr;
				//method.declaringTypeDef = type.aotTypeDef;
				//method.name = _rawImage->GetStringFromRawIndex(data.name);
				if (type.aotTypeDef != nullptr)
				{
					MethodRefSig signature = {};
					signature.flags = data.flags;
					BlobReader methodSigReader = _rawImage->GetBlobReaderByRawIndex(data.signature);
					ReadMethodDefSig(methodSigReader, signature);
					const char* methodName = _rawImage->GetStringFromRawIndex(data.name);
					method.aotMethodDef = FindMatchMethod(type.aotTypeDef, method, methodName, signature);
				}
				if (!method.aotMethodDef && _interpreterFallbackImage)
				{
					method.aotMethodDef = _interpreterFallbackImage->GetMethodDefinitionFromRawIndex(i - 1);
					method.interpreterFallback = true;
					if (type.aotTypeDef)
					{
						Il2CppClass* baseClass = il2cpp::vm::Class::FromIl2CppType(type.aotIl2CppType);
						MethodInfo* alias = static_cast<MethodInfo*>(
							HYBRIDCLR_METADATA_MALLOC(sizeof(MethodInfo)));
						*alias = *currentMethod;
						alias->klass = baseClass;
						_supplementalMethods[baseClass].push_back(alias);
						_supplementalMethodImages[alias] = _interpreterFallbackImage;
						logicalMethod = alias;
					}
				}
				if (method.aotMethodDef && !method.interpreterFallback)
				{
					_token2MethodDefs[method.aotMethodDef->token] = &method;
					logicalMethod = FindRuntimeMethod(type.aotIl2CppType, method.aotMethodDef);
					_customAttributeTokens[method.aotMethodDef->token] =
						currentMethod ? currentMethod->token : EncodeToken(TableType::METHOD, i);
				}
				if (currentMethod && logicalMethod)
				{
					_logicalMethods[currentMethod] = logicalMethod;
					if (!dhe::RegisterLogicalMethodMapping(_targetAssembly,
						currentMethod, logicalMethod))
					{
						RaiseExecutionEngineException(
							"DHE logical method identity registration failed.");
					}
				}
			}
		}
	}

	void SuperSetAOTHomologousImage::ReadMethodDefSig(BlobReader& reader, MethodRefSig& method)
	{
		uint8_t rawSigFlags = reader.ReadByte();

		if (rawSigFlags & (uint8_t)MethodSigFlags::GENERIC)
		{
			//IL2CPP_ASSERT(false);
			method.genericParamCount = reader.ReadCompressedUint32();
			IL2CPP_ASSERT(method.genericParamCount > 0);
		}
		uint32_t paramCount = reader.ReadCompressedUint32();
		//IL2CPP_ASSERT(paramCount >= methodDef.parameterCount);

		method.returnType = ReadType(reader, nullptr, nullptr);

		int readParamNum = 0;
		for (; reader.NonEmpty(); )
		{
			const Il2CppType* paramType = ReadType(reader, nullptr, nullptr);
			method.params.push_back(paramType);
			++readParamNum;
		}
		IL2CPP_ASSERT(readParamNum == (int)paramCount);
	}

	void SuperSetAOTHomologousImage::InitFields(std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos)
	{
		const Table& fieldTb = _rawImage->GetTable(TableType::FIELD);
		uint32_t fieldCount = fieldTb.rowNum;
		_fields.resize(fieldTb.rowNum);

		uint32_t typeCount = (uint32_t)typeIntermediateInfos.size();
		for (SuperSetTypeIntermediateInfo& type : typeIntermediateInfos)
		{
			uint32_t nextTypeIndex = (uint32_t)(&type - &typeIntermediateInfos[0] + 1);
			uint32_t nextTypeFieldStartIndex = nextTypeIndex < typeCount ? typeIntermediateInfos[nextTypeIndex].homoFieldStartIndex : fieldCount + 1;
			for (uint32_t i = type.homoFieldStartIndex; i < nextTypeFieldStartIndex; i++)
			{
				SuperSetFieldDefDetail& field = _fields[i - 1];
				//field.homoRowIndex = i;
				TbField data = _rawImage->ReadField(i);
				//field.name = _rawImage->GetStringFromRawIndex(data.name);

				//field.declaringTypeDef = type.aotTypeDef;
				field.declaringIl2CppType = type.aotIl2CppType;
				const Il2CppType* logicalFieldType = nullptr;
				if (type.aotTypeDef != nullptr)
				{
					BlobReader br = _rawImage->GetBlobReaderByRawIndex(data.signature);
					FieldRefSig frs;
					ReadFieldRefSig(br, nullptr, frs);
					if (data.flags)
					{
						Il2CppType* newType = MetadataPool::ShallowCloneIl2CppType(frs.type);
						newType->attrs = data.flags;
						frs.type = newType;
					}
					logicalFieldType = frs.type;

					const char* fieldName = _rawImage->GetStringFromRawIndex(data.name);
					field.aotFieldDef = FindMatchField(type.aotTypeDef, field, fieldName, frs.type);
					if (field.aotFieldDef)
					{
						_matchedAotFieldTokens.insert(field.aotFieldDef->token);
						_customAttributeTokens[field.aotFieldDef->token] =
							EncodeToken(TableType::FIELD, i);
					}
				}
				if (!field.aotFieldDef && _interpreterFallbackImage)
				{
					field.aotFieldDef = _interpreterFallbackImage->GetFieldDefinitionFromRawIndex(i - 1);
					const uint32_t rawTypeIndex = static_cast<uint32_t>(
						&type - &typeIntermediateInfos[0]);
					field.declaringIl2CppType =
						_interpreterFallbackImage->GetIl2CppTypeFromRawTypeDefIndex(rawTypeIndex);
					field.interpreterFallback = true;
					if (type.aotTypeDef)
					{
						Il2CppClass* baseClass =
							il2cpp::vm::Class::FromIl2CppType(type.aotIl2CppType);
						FieldInfo* fallbackField = const_cast<FieldInfo*>(
							GetFieldInfoFromFieldRef(*field.declaringIl2CppType,
								field.aotFieldDef));
						FieldInfo* logicalField = fallbackField;
						if (logicalFieldType)
						{
							logicalField = static_cast<FieldInfo*>(
								HYBRIDCLR_METADATA_MALLOC(sizeof(FieldInfo)));
							*logicalField = *fallbackField;
							logicalField->type = logicalFieldType;
						}
						_supplementalFields[baseClass].push_back(logicalField);
						_supplementalFieldLogicalParents[logicalField] = baseClass;
						if ((data.flags & FIELD_ATTRIBUTE_STATIC) == 0)
						{
							if (baseClass->byval_arg.valuetype ||
								type.aotTypeDef->genericContainerIndex != kGenericContainerIndexInvalid ||
								logicalField->type->byref || logicalField->type->type == IL2CPP_TYPE_PTR ||
								logicalField->type->type == IL2CPP_TYPE_FNPTR ||
								logicalField->type->type == IL2CPP_TYPE_TYPEDBYREF)
							{
								TEMP_FORMAT(errMsg, "unsupported DHE supplemental instance field: %s::%s",
									baseClass->name, fallbackField->name);
								RaiseExecutionEngineException(errMsg);
							}
							MetadataModule::RegisterDheSupplementalInstanceField(
								fallbackField, logicalField);
						}
					}
				}
			}
		}

		for (SuperSetTypeIntermediateInfo& type : typeIntermediateInfos)
		{
			if (!type.aotTypeDef || !type.aotIl2CppType)
			{
				continue;
			}
			Il2CppClass* baseClass = il2cpp::vm::Class::FromIl2CppType(type.aotIl2CppType);
			il2cpp::vm::Class::SetupFields(baseClass);
			for (uint16_t index = 0; index < baseClass->field_count; ++index)
			{
				FieldInfo* field = baseClass->fields + index;
				if (_matchedAotFieldTokens.find(field->token) == _matchedAotFieldTokens.end())
				{
					_removedFields.insert(field);
				}
			}
		}
	}

	const MethodInfo* SuperSetAOTHomologousImage::GetLogicalMethod(
		const MethodInfo* currentMethod)
	{
		if (!currentMethod)
		{
			return nullptr;
		}
		auto logical = _logicalMethods.find(currentMethod);
		if (logical == _logicalMethods.end())
		{
			TEMP_FORMAT(errMsg, "DHE logical accessor was not registered: %s::%s",
				currentMethod->klass->name, currentMethod->name);
			RaiseExecutionEngineException(errMsg);
		}
		return logical->second;
	}

	void SuperSetAOTHomologousImage::InitPropertiesAndEvents(
		std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos)
	{
		if (!_interpreterFallbackImage)
		{
			return;
		}

		for (uint32_t rawTypeIndex = 1;
			rawTypeIndex < static_cast<uint32_t>(typeIntermediateInfos.size()); ++rawTypeIndex)
		{
			SuperSetTypeIntermediateInfo& type = typeIntermediateInfos[rawTypeIndex];
			if (!type.aotTypeDef || !type.aotIl2CppType)
			{
				continue;
			}

			Il2CppClass* baseClass = il2cpp::vm::Class::FromIl2CppType(type.aotIl2CppType);
			Il2CppClass* currentClass = _interpreterFallbackImage
				->GetTypeInfoFromTypeDefinitionRawIndex(rawTypeIndex);
			std::vector<const PropertyInfo*>& logicalProperties = _logicalProperties[baseClass];
			std::vector<const EventInfo*>& logicalEvents = _logicalEvents[baseClass];

			il2cpp::vm::Class::SetupProperties(currentClass);
			for (uint16_t index = 0; index < currentClass->property_count; ++index)
			{
#if UNITY_ENGINE_TUANJIE
				const PropertyInfo* currentProperty = currentClass->properties[index];
#else
				const PropertyInfo* currentProperty = currentClass->properties + index;
#endif
				PropertyInfo* logicalProperty = static_cast<PropertyInfo*>(
					HYBRIDCLR_METADATA_MALLOC(sizeof(PropertyInfo)));
				*logicalProperty = *currentProperty;
				logicalProperty->parent = baseClass;
				logicalProperty->get = GetLogicalMethod(currentProperty->get);
				logicalProperty->set = GetLogicalMethod(currentProperty->set);
				logicalProperties.push_back(logicalProperty);
			}

			il2cpp::vm::Class::SetupEvents(currentClass);
			for (uint16_t index = 0; index < currentClass->event_count; ++index)
			{
				const EventInfo* currentEvent = currentClass->events + index;
				EventInfo* logicalEvent = static_cast<EventInfo*>(
					HYBRIDCLR_METADATA_MALLOC(sizeof(EventInfo)));
				*logicalEvent = *currentEvent;
				logicalEvent->parent = baseClass;
				logicalEvent->add = GetLogicalMethod(currentEvent->add);
				logicalEvent->remove = GetLogicalMethod(currentEvent->remove);
				logicalEvent->raise = GetLogicalMethod(currentEvent->raise);
				logicalEvents.push_back(logicalEvent);
			}
		}
	}

	MethodBody* SuperSetAOTHomologousImage::GetMethodBody(uint32_t token)
	{
		auto it = _token2MethodDefs.find(token);
		if (it == _token2MethodDefs.end())
		{
			return nullptr;
		}
		SuperSetMethodDefDetail* method = it->second;
		if (method->interpreterFallback && _interpreterFallbackImage)
		{
			return _interpreterFallbackImage->GetMethodBody(token);
		}
		uint32_t rowIndex = (uint32_t)(method - &_methodDefs[0] + 1);
		TbMethod methodData = _rawImage->ReadMethod(rowIndex);
		MethodBody* body = new (HYBRIDCLR_MALLOC_ZERO(sizeof(MethodBody))) MethodBody();
		ReadMethodBody(*method->aotMethodDef, methodData, *body);
		return body;
	}

	const Il2CppType* SuperSetAOTHomologousImage::GetIl2CppTypeFromRawTypeDefIndex(uint32_t index)
	{
		IL2CPP_ASSERT((size_t)index < _typeDefs.size());
		return _typeDefs[index].aotIl2CppType;
	}

	Il2CppGenericContainer* SuperSetAOTHomologousImage::GetGenericContainerByRawIndex(uint32_t index)
	{
		return (Il2CppGenericContainer*)il2cpp::vm::GlobalMetadata::GetGenericContainerFromIndex(index);
	}

	Il2CppGenericContainer* SuperSetAOTHomologousImage::GetGenericContainerByTypeDefRawIndex(int32_t typeDefIndex)
	{
		auto it = _aotTypeIndex2TypeDefs.find(typeDefIndex);
		if (it == _aotTypeIndex2TypeDefs.end())
		{
			return nullptr;
		}
		const Il2CppType* type = it->second->aotIl2CppType;
		if (type == nullptr)
		{
			return nullptr;
		}
		const Il2CppTypeDefinition* typeDef = (const Il2CppTypeDefinition*)(type->data.typeHandle);
		return (Il2CppGenericContainer*)il2cpp::vm::GlobalMetadata::GetGenericContainerFromIndex(typeDef->genericContainerIndex);
	}

	const Il2CppMethodDefinition* SuperSetAOTHomologousImage::GetMethodDefinitionFromRawIndex(uint32_t index)
	{
		IL2CPP_ASSERT((size_t)index < _methodDefs.size());
		SuperSetMethodDefDetail& method = _methodDefs[index];
		const Il2CppMethodDefinition* methodDef = method.aotMethodDef;
		if (!methodDef)
		{
			TEMP_FORMAT(errMsg, "method not exist. rowIndex:%d", index);
			RaiseExecutionEngineException(errMsg);
		}
		return methodDef;
	}

	void SuperSetAOTHomologousImage::ReadFieldRefInfoFromFieldDefToken(uint32_t rowIndex, FieldRefInfo& ret)
	{
		IL2CPP_ASSERT(rowIndex > 0);
		SuperSetFieldDefDetail& fd = _fields[rowIndex - 1];
		ret.containerType = fd.declaringIl2CppType;
		ret.field = fd.aotFieldDef;
	}

	const Il2CppType* SuperSetAOTHomologousImage::ReadTypeFromResolutionScope(uint32_t scope, uint32_t typeNamespace, uint32_t typeName)
	{
		TableType tokenType;
		uint32_t rawIndex;
		DecodeResolutionScopeCodedIndex(scope, tokenType, rawIndex);
		switch (tokenType)
		{
		case TableType::MODULE:
		{
			const Il2CppType* retType = GetModuleIl2CppType(rawIndex, typeNamespace, typeName, false);
			return retType ? retType : _defaultIl2CppType;
		}
		case TableType::MODULEREF:
		{
			RaiseNotSupportedException("Image::ReadTypeFromResolutionScope not support ResolutionScore.MODULEREF");
			return nullptr;
		}
		case TableType::ASSEMBLYREF:
		{
			const Il2CppType* refType = GetIl2CppType(rawIndex, typeNamespace, typeName, false);
			return refType ? refType : _defaultIl2CppType;
		}
		case TableType::TYPEREF:
		{
			const Il2CppType* enClosingType = ReadTypeFromTypeRef(rawIndex);
			IL2CPP_ASSERT(typeNamespace == 0);
			const char* name = _rawImage->GetStringFromRawIndex(typeName);

			void* iter = nullptr;
			Il2CppMetadataTypeHandle enclosingTypeDef = enClosingType->data.typeHandle;
			if (!enclosingTypeDef)
			{
				//TEMP_FORMAT(errMsg, "Image::ReadTypeFromResolutionScope ReadTypeFromResolutionScope.TYPEREF enclosingType:%s", name);
				//RaiseExecutionEngineException(errMsg);
				return _defaultIl2CppType;
			}
			for (const Il2CppTypeDefinition* nextTypeDef; (nextTypeDef = (const Il2CppTypeDefinition*)il2cpp::vm::GlobalMetadata::GetNestedTypes(enclosingTypeDef, &iter));)
			{
				const char* nestedTypeName = il2cpp::vm::GlobalMetadata::GetStringFromIndex(nextTypeDef->nameIndex);
				IL2CPP_ASSERT(nestedTypeName);
				if (!std::strcmp(name, nestedTypeName))
				{
					return GetIl2CppTypeFromTypeDefinition(nextTypeDef);
				}
			}
			return _defaultIl2CppType;
		}
		default:
		{
			RaiseBadImageException("Image::ReadTypeFromResolutionScope invaild TableType");
			return nullptr;
		}
		}
	}

	Il2CppClass* SuperSetAOTHomologousImage::FindSupplementalType(const char* namespaze,
		const char* name)
	{
		for (Il2CppClass* klass : _supplementalTypes)
		{
			if (klass && !klass->declaringType &&
				!std::strcmp(klass->namespaze, namespaze) &&
				!std::strcmp(klass->name, name))
			{
				return klass;
			}
		}
		return nullptr;
	}

	void SuperSetAOTHomologousImage::GetSupplementalTypes(
		std::vector<const Il2CppClass*>& types)
	{
		types.insert(types.end(), _supplementalTypes.begin(), _supplementalTypes.end());
	}

	Il2CppClass* SuperSetAOTHomologousImage::GetFirstSupplementalNestedType(
		Il2CppClass* klass, void** iter)
	{
		auto types = _supplementalNestedTypes.find(klass);
		if (types == _supplementalNestedTypes.end() || types->second.empty())
		{
			return nullptr;
		}
		*iter = &types->second[0];
		return types->second[0];
	}

	bool SuperSetAOTHomologousImage::TryGetNextSupplementalNestedType(
		Il2CppClass* klass, void** iter, Il2CppClass** nestedType)
	{
		auto types = _supplementalNestedTypes.find(klass);
		if (types == _supplementalNestedTypes.end() || types->second.empty() || !*iter)
		{
			return false;
		}
		const uintptr_t current = reinterpret_cast<uintptr_t>(*iter);
		const uintptr_t begin = reinterpret_cast<uintptr_t>(&types->second[0]);
		const uintptr_t end = reinterpret_cast<uintptr_t>(
			&types->second[0] + types->second.size());
		if (current < begin || current >= end ||
			(current - begin) % sizeof(Il2CppClass*) != 0)
		{
			return false;
		}
		const size_t nextIndex = (current - begin) / sizeof(Il2CppClass*) + 1;
		if (nextIndex >= types->second.size())
		{
			*nestedType = nullptr;
			return true;
		}
		*iter = &types->second[nextIndex];
		*nestedType = types->second[nextIndex];
		return true;
	}

	const MethodInfo* SuperSetAOTHomologousImage::GetFirstSupplementalMethod(
		Il2CppClass* klass, void** iter)
	{
		auto methods = _supplementalMethods.find(klass);
		if (methods == _supplementalMethods.end() || methods->second.empty())
		{
			return nullptr;
		}
		*iter = &methods->second[0];
		return methods->second[0];
	}

	bool SuperSetAOTHomologousImage::TryGetNextSupplementalMethod(Il2CppClass* klass,
		void** iter, const MethodInfo** method)
	{
		auto methods = _supplementalMethods.find(klass);
		if (methods == _supplementalMethods.end() || methods->second.empty() || !*iter)
		{
			return false;
		}
		const uintptr_t current = reinterpret_cast<uintptr_t>(*iter);
		const uintptr_t begin = reinterpret_cast<uintptr_t>(&methods->second[0]);
		const uintptr_t end = reinterpret_cast<uintptr_t>(
			&methods->second[0] + methods->second.size());
		if (current < begin || current >= end ||
			(current - begin) % sizeof(const MethodInfo*) != 0)
		{
			return false;
		}
		const size_t nextIndex = (current - begin) / sizeof(const MethodInfo*) + 1;
		if (nextIndex >= methods->second.size())
		{
			*method = nullptr;
			return true;
		}
		*iter = &methods->second[nextIndex];
		*method = methods->second[nextIndex];
		return true;
	}

	Image* SuperSetAOTHomologousImage::GetSupplementalMethodImage(const MethodInfo* method)
	{
		if (method->is_inflated)
		{
			method = method->genericMethod->methodDefinition;
		}
		auto image = _supplementalMethodImages.find(method);
		return image == _supplementalMethodImages.end() ? nullptr : image->second;
	}

	Image* SuperSetAOTHomologousImage::GetMethodResolveImage(const MethodInfo* method)
	{
		if (method->is_inflated)
		{
			method = method->genericMethod->methodDefinition;
		}
		// New types can also refer back to Base types. Their bodies must use the
		// merged view, or typeof/casts/calls see a second hidden current class.
		return _supplementalMethodImages.find(method) != _supplementalMethodImages.end() ||
			_logicalMethods.find(method) != _logicalMethods.end() ||
			(_interpreterFallbackImage &&
				method->klass->image == _interpreterFallbackImage->GetIl2CppImage())
			? this : nullptr;
	}

	size_t SuperSetAOTHomologousImage::GetSupplementalMethodCount(Il2CppClass* klass)
	{
		auto methods = _supplementalMethods.find(klass);
		return methods == _supplementalMethods.end() ? 0 : methods->second.size();
	}

	FieldInfo* SuperSetAOTHomologousImage::GetFirstSupplementalField(
		Il2CppClass* klass, void** iter)
	{
		auto fields = _supplementalFields.find(klass);
		if (fields == _supplementalFields.end() || fields->second.empty())
		{
			return nullptr;
		}
		*iter = &fields->second[0];
		return fields->second[0];
	}

	bool SuperSetAOTHomologousImage::TryGetNextSupplementalField(Il2CppClass* klass,
		void** iter, FieldInfo** field)
	{
		auto fields = _supplementalFields.find(klass);
		if (fields == _supplementalFields.end() || fields->second.empty() || !*iter)
		{
			return false;
		}
		const uintptr_t current = reinterpret_cast<uintptr_t>(*iter);
		const uintptr_t begin = reinterpret_cast<uintptr_t>(&fields->second[0]);
		const uintptr_t end = reinterpret_cast<uintptr_t>(
			&fields->second[0] + fields->second.size());
		if (current < begin || current >= end ||
			(current - begin) % sizeof(FieldInfo*) != 0)
		{
			return false;
		}
		const size_t nextIndex = (current - begin) / sizeof(FieldInfo*) + 1;
		if (nextIndex >= fields->second.size())
		{
			*field = nullptr;
			return true;
		}
		*iter = &fields->second[nextIndex];
		*field = fields->second[nextIndex];
		return true;
	}

	size_t SuperSetAOTHomologousImage::GetSupplementalFieldCount(Il2CppClass* klass)
	{
		auto fields = _supplementalFields.find(klass);
		return fields == _supplementalFields.end() ? 0 : fields->second.size();
	}

	bool SuperSetAOTHomologousImage::IsRemovedField(const FieldInfo* field)
	{
		return _removedFields.find(field) != _removedFields.end();
	}

	Il2CppClass* SuperSetAOTHomologousImage::GetSupplementalFieldLogicalParent(
		const FieldInfo* field)
	{
		auto parent = _supplementalFieldLogicalParents.find(field);
		return parent == _supplementalFieldLogicalParents.end() ? nullptr : parent->second;
	}

	bool SuperSetAOTHomologousImage::TryGetCustomAttributeSource(uint32_t token,
		const Il2CppImage*& sourceImage, uint32_t& sourceToken)
	{
		auto currentToken = _customAttributeTokens.find(token);
		if (currentToken == _customAttributeTokens.end() || !_interpreterFallbackImage)
		{
			return false;
		}
		sourceImage = _interpreterFallbackImage->GetIl2CppImage();
		sourceToken = currentToken->second;
		return sourceImage != nullptr;
	}

	bool SuperSetAOTHomologousImage::HasLogicalPropertyView(Il2CppClass* klass)
	{
		return _logicalProperties.find(klass) != _logicalProperties.end();
	}

	const PropertyInfo* SuperSetAOTHomologousImage::GetFirstLogicalProperty(
		Il2CppClass* klass, void** iter)
	{
		auto properties = _logicalProperties.find(klass);
		if (properties == _logicalProperties.end() || properties->second.empty())
		{
			return nullptr;
		}
		*iter = &properties->second[0];
		return properties->second[0];
	}

	bool SuperSetAOTHomologousImage::TryGetNextLogicalProperty(Il2CppClass* klass,
		void** iter, const PropertyInfo** property)
	{
		auto properties = _logicalProperties.find(klass);
		if (properties == _logicalProperties.end() || properties->second.empty() || !*iter)
		{
			return false;
		}
		const uintptr_t current = reinterpret_cast<uintptr_t>(*iter);
		const uintptr_t begin = reinterpret_cast<uintptr_t>(&properties->second[0]);
		const uintptr_t end = reinterpret_cast<uintptr_t>(
			&properties->second[0] + properties->second.size());
		if (current < begin || current >= end ||
			(current - begin) % sizeof(const PropertyInfo*) != 0)
		{
			return false;
		}
		const size_t nextIndex = (current - begin) / sizeof(const PropertyInfo*) + 1;
		if (nextIndex >= properties->second.size())
		{
			*property = nullptr;
			return true;
		}
		*iter = &properties->second[nextIndex];
		*property = properties->second[nextIndex];
		return true;
	}

	size_t SuperSetAOTHomologousImage::GetLogicalPropertyCount(Il2CppClass* klass)
	{
		auto properties = _logicalProperties.find(klass);
		return properties == _logicalProperties.end() ? 0 : properties->second.size();
	}

	bool SuperSetAOTHomologousImage::HasLogicalEventView(Il2CppClass* klass)
	{
		return _logicalEvents.find(klass) != _logicalEvents.end();
	}

	const EventInfo* SuperSetAOTHomologousImage::GetFirstLogicalEvent(
		Il2CppClass* klass, void** iter)
	{
		auto events = _logicalEvents.find(klass);
		if (events == _logicalEvents.end() || events->second.empty())
		{
			return nullptr;
		}
		*iter = &events->second[0];
		return events->second[0];
	}

	bool SuperSetAOTHomologousImage::TryGetNextLogicalEvent(Il2CppClass* klass,
		void** iter, const EventInfo** eventInfo)
	{
		auto events = _logicalEvents.find(klass);
		if (events == _logicalEvents.end() || events->second.empty() || !*iter)
		{
			return false;
		}
		const uintptr_t current = reinterpret_cast<uintptr_t>(*iter);
		const uintptr_t begin = reinterpret_cast<uintptr_t>(&events->second[0]);
		const uintptr_t end = reinterpret_cast<uintptr_t>(
			&events->second[0] + events->second.size());
		if (current < begin || current >= end ||
			(current - begin) % sizeof(const EventInfo*) != 0)
		{
			return false;
		}
		const size_t nextIndex = (current - begin) / sizeof(const EventInfo*) + 1;
		if (nextIndex >= events->second.size())
		{
			*eventInfo = nullptr;
			return true;
		}
		*iter = &events->second[nextIndex];
		*eventInfo = events->second[nextIndex];
		return true;
	}

	size_t SuperSetAOTHomologousImage::GetLogicalEventCount(Il2CppClass* klass)
	{
		auto events = _logicalEvents.find(klass);
		return events == _logicalEvents.end() ? 0 : events->second.size();
	}
}
}
