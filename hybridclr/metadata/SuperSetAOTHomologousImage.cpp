#include "SuperSetAOTHomologousImage.h"

#include "vm/MetadataLock.h"
#include "vm/GlobalMetadata.h"
#include "vm/Class.h"
#include "vm/GenericClass.h"
#include "vm/Image.h"
#include "vm/Exception.h"
#include "vm/MetadataCache.h"
#include "metadata/GenericMetadata.h"
#include "MetadataPool.h"
#include "InterpreterImage.h"
#include "MetadataModule.h"
#include "DheGenericFieldMetadata.h"
#include "DheInterfaceSlots.h"

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
		if (_typeIntermediateInfos.empty())
		{
			InitTypeReferences();
		}
		InitSupplementalTypes(_typeIntermediateInfos);
		InitMethods(_typeIntermediateInfos);
		InitFields(_typeIntermediateInfos);
		InitPropertiesAndEvents(_typeIntermediateInfos);
		_typeIntermediateInfos.clear();
		_typeIntermediateInfos.shrink_to_fit();
	}

	void SuperSetAOTHomologousImage::InitTypeReferences()
	{
		_defaultIl2CppType = &il2cpp_defaults.missing_class->byval_arg;

		InitTypes0(_typeIntermediateInfos);
		InitNestedClass(_typeIntermediateInfos);
		InitTypes1(_typeIntermediateInfos);
	}

	const Il2CppType* SuperSetAOTHomologousImage::FindTypeReference(const char* namespaze, const char* name)
	{
		auto entry = _typeReferencesByName.find(std::string(namespaze).append(1, '\0').append(name));
		return entry == _typeReferencesByName.end() ? nullptr : entry->second;
	}

	bool SuperSetAOTHomologousImage::SetCurrentImagePlan(const dhe::CurrentImagePlan& plan)
	{
		if (!_isDheImage || !_interpreterFallbackImage || !_targetAssembly ||
			!_targetAssembly->aname.name || plan.assemblyName != _targetAssembly->aname.name ||
			!_typeDefs.empty() || !_currentImagePlan.assemblyName.empty())
			return false;
		const uint32_t typeCount = _rawImage->GetTable(TableType::TYPEDEF).rowNum;
		const uint32_t methodCount = _rawImage->GetTable(TableType::METHOD).rowNum;
		std::unordered_map<uint32_t, uint32_t> selectedTypes;
		std::unordered_set<uint32_t> baseTypes, baseMethods, currentMethods;
		for (const dhe::CurrentMetadataTokenBinding& binding : plan.types)
		{
			if (DecodeTokenTableType(binding.currentToken) != TableType::TYPEDEF ||
				DecodeTokenTableType(binding.baseToken) != TableType::TYPEDEF ||
				DecodeTokenRowIndex(binding.currentToken) <= 1 ||
				DecodeTokenRowIndex(binding.currentToken) > typeCount ||
				DecodeTokenRowIndex(binding.baseToken) <= 1 ||
				!baseTypes.insert(binding.baseToken).second ||
				!selectedTypes.emplace(binding.currentToken, binding.baseToken).second)
				return false;
		}
		for (const dhe::CurrentMetadataTokenBinding& binding : plan.methods)
		{
			if (DecodeTokenTableType(binding.currentToken) != TableType::METHOD ||
				DecodeTokenTableType(binding.baseToken) != TableType::METHOD ||
				DecodeTokenRowIndex(binding.currentToken) == 0 ||
				DecodeTokenRowIndex(binding.currentToken) > methodCount ||
				DecodeTokenRowIndex(binding.baseToken) == 0 ||
				!baseMethods.insert(binding.baseToken).second ||
				!currentMethods.insert(binding.currentToken).second)
				return false;
		}
		_currentImagePlan = plan;
		_currentStorageTypeTokens.swap(selectedTypes);
		return true;
	}

	bool SuperSetAOTHomologousImage::AppendDheCurrentExecutions(dhe::MetaVersionRegistration& registration)
	{
		if (_currentImagePlan.assemblyName.empty()) return true;
		if (registration.baseAssembly != _targetAssembly || !registration.baseMetaVersion ||
			!registration.currentMetaVersion ||
			registration.baseMetaVersion->assemblyHash != _currentImagePlan.baseAssemblyHash ||
			registration.currentMetaVersion->assemblyHash != _currentImagePlan.currentAssemblyHash)
			return false;
		std::vector<uint32_t> typeTokens, methodTokens;
		std::unordered_map<uint32_t, uint32_t> providedMethods;
		for (const dhe::CurrentMetadataTokenBinding& binding : _currentImagePlan.types) typeTokens.push_back(binding.currentToken);
		for (const dhe::CurrentMetadataTokenBinding& binding : _currentImagePlan.methods)
		{
			methodTokens.push_back(binding.currentToken);
			providedMethods.emplace(binding.currentToken, binding.baseToken);
		}
		dhe::CurrentImagePlan checked;
		if (!dhe::BuildCurrentImagePlan(*registration.baseMetaVersion, *registration.currentMetaVersion,
			typeTokens, methodTokens, checked, _currentImagePlan.source) || checked.methods.size() != _currentImagePlan.methods.size())
			return false;
		// Recheck the whole MV identity selection before dispatch publication,
		// including implicit members of a selected Current storage type.
		for (const dhe::CurrentMetadataTokenBinding& binding : checked.types)
			if (_currentStorageTypeTokens.at(binding.currentToken) != binding.baseToken) return false;
		std::vector<dhe::CurrentMethodExecution> executions(registration.currentExecutions);
		for (const dhe::CurrentMetadataTokenBinding& binding : checked.methods)
		{
			if (providedMethods.at(binding.currentToken) != binding.baseToken) return false;
			const MethodInfo* current = _interpreterFallbackImage->GetMethodInfoFromMethodDefinitionRawIndex(
				DecodeTokenRowIndex(binding.currentToken) - 1);
			auto logical = _logicalMethods.find(current);
			if (logical == _logicalMethods.end() || !logical->second ||
				logical->second->klass->image != _targetAssembly->image ||
				logical->second->token != binding.baseToken)
				return false;
			executions.emplace_back(binding.baseToken, current);
		}
		registration.currentExecutions.swap(executions);
		registration.source = _currentImagePlan.source;
		return true;
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
			type.aotIl2CppType = _interpreterFallbackImage->GetRawTypeDefinitionType(rawTypeIndex);
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
			if (_isDheImage && rawTypeIndex != 0 && td.homoParentRowIndex == 0)
			{
				TbTypeDef data = _rawImage->ReadTypeDef(rawTypeIndex + 1);
				std::string key = std::string(_rawImage->GetStringFromRawIndex(data.typeNamespace))
					.append(1, '\0').append(_rawImage->GetStringFromRawIndex(data.typeName));
				if (!_typeReferencesByName.emplace(std::move(key), td.aotIl2CppType).second)
					RaiseBadImageException("duplicate top-level type definition");
			}
			if (td.aotTypeDef)
			{
				_aotTypeIndex2TypeDefs[il2cpp::vm::GlobalMetadata::GetIndexForTypeDefinition(td.aotTypeDef)] = &type;
				_customAttributeTokens[td.aotTypeDef->token] =
					EncodeToken(TableType::TYPEDEF, rawTypeIndex + 1);
			}
			auto selected = _currentStorageTypeTokens.find(EncodeToken(TableType::TYPEDEF, rawTypeIndex + 1));
			if (selected != _currentStorageTypeTokens.end() &&
				(!td.aotTypeDef || td.aotTypeDef->token != selected->second ||
				 td.aotIl2CppType->type != _interpreterFallbackImage->GetRawTypeDefinitionType(rawTypeIndex)->type))
				RaiseBadImageException("DHE Current storage selection does not match the Base type identity.");
		}
	}

	void SuperSetAOTHomologousImage::InitSupplementalTypes(
		std::vector<SuperSetTypeIntermediateInfo>& typeIntermediateInfos)
	{
		if (!_interpreterFallbackImage)
		{
			return;
		}
		for (uint32_t rawTypeIndex = 1; rawTypeIndex < typeIntermediateInfos.size(); ++rawTypeIndex)
		{
			SuperSetTypeIntermediateInfo& td = typeIntermediateInfos[rawTypeIndex];
			if (!td.aotTypeDef)
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
			const bool currentStorage = _currentStorageTypeTokens.count(EncodeToken(TableType::TYPEDEF, nextTypeIndex)) != 0;
			Il2CppClass* baseInterface = _isDheImage && type.aotTypeDef && IsInterface(type.aotTypeDef->flags)
				? il2cpp::vm::Class::FromIl2CppType(type.aotIl2CppType) : nullptr;
			if (baseInterface)
				_interfaceMethods[baseInterface].resize(baseInterface->method_count, nullptr);


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
						logicalMethod = currentMethod;
						if (!currentStorage)
						{
							MethodInfo* alias = static_cast<MethodInfo*>(HYBRIDCLR_METADATA_MALLOC(sizeof(MethodInfo)));
							*alias = *currentMethod;
							alias->klass = baseClass;
							logicalMethod = alias;
						}
						_supplementalMethods[baseClass].push_back(logicalMethod);
						_supplementalMethodImages[logicalMethod] = _interpreterFallbackImage;
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
					if (baseInterface && IsVirtualMethod(currentMethod->flags))
					{
						uint16_t slot;
						if (!BindDheInterfaceSlot(_interfaceMethods[baseInterface],
							method.interpreterFallback ? UINT16_MAX : logicalMethod->slot, currentMethod, slot))
							RaiseBadImageException("Invalid DHE interface slot mapping.");
						if (method.interpreterFallback)
							const_cast<MethodInfo*>(logicalMethod)->slot = slot;
					}
					_logicalMethods[currentMethod] = logicalMethod;
					_currentMetadataMethods[logicalMethod] = currentMethod;
					if (currentMethod != logicalMethod && !dhe::RegisterLogicalMethodMapping(_targetAssembly,
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

	void SuperSetAOTHomologousImage::SelectCurrentStaticValueField(SuperSetFieldDefDetail& field,
		const SuperSetTypeIntermediateInfo& type, uint32_t rawTypeIndex, uint32_t rawFieldIndex)
	{
		if (!_isDheImage || !_interpreterFallbackImage || !HasCurrentImagePlan() || !field.aotFieldDef)
			return;
		const Il2CppType* logicalType = il2cpp::vm::GlobalMetadata::GetIl2CppTypeFromIndex(field.aotFieldDef->typeIndex);
		if (!(logicalType->attrs & FIELD_ATTRIBUTE_STATIC) || (logicalType->attrs & FIELD_ATTRIBUTE_LITERAL))
			return;
		const Il2CppType* physicalOwner = _interpreterFallbackImage->GetRawTypeDefinitionType(rawTypeIndex);
		const Il2CppFieldDefinition* physicalDefinition = _interpreterFallbackImage->GetFieldDefinitionFromRawIndex(rawFieldIndex);
		FieldInfo* physical = const_cast<FieldInfo*>(GetFieldInfoFromFieldRef(*physicalOwner, physicalDefinition));
		if (!physical->type->valuetype || physical->type->byref ||
			IsMatchSigType(logicalType, physical->type, GetGenericContainerFromIl2CppType(type.aotIl2CppType), nullptr))
			return;

		// The Current allocation and GC descriptor own the entire evolved value.
		// Other static fields continue to use their original Base addresses.
		Il2CppClass* baseClass = il2cpp::vm::Class::FromIl2CppType(type.aotIl2CppType);
		il2cpp::vm::Class::SetupFields(baseClass);
		for (uint16_t index = 0; index < baseClass->field_count; ++index)
			if (baseClass->fields[index].token == field.aotFieldDef->token)
				_logicalFields[baseClass->fields + index] = physical;
		_matchedAotFieldTokens.erase(field.aotFieldDef->token);
		_supplementalFields[baseClass].push_back(physical);
		_supplementalFieldLogicalParents[physical] = baseClass;
		_logicalFields[physical] = physical;
		field.declaringIl2CppType = physicalOwner;
		field.aotFieldDef = physicalDefinition;
		field.interpreterFallback = true;
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
				if (_currentStorageTypeTokens.count(EncodeToken(TableType::TYPEDEF, nextTypeIndex)))
				{
					// Inline values and selected containing objects own real Current
					// fields. A Base offset or a reference sidecar cannot represent a
					// larger value, independent value copies or array element stride.
					field.declaringIl2CppType = _interpreterFallbackImage->GetRawTypeDefinitionType(nextTypeIndex - 1);
					field.aotFieldDef = _interpreterFallbackImage->GetFieldDefinitionFromRawIndex(i - 1);
					field.interpreterFallback = true;
					Il2CppClass* baseClass = il2cpp::vm::Class::FromIl2CppType(type.aotIl2CppType);
					FieldInfo* physical = const_cast<FieldInfo*>(GetFieldInfoFromFieldRef(
						*field.declaringIl2CppType, field.aotFieldDef));
					_supplementalFields[baseClass].push_back(physical);
					_supplementalFieldLogicalParents[physical] = baseClass;
					_logicalFields[physical] = physical;
					continue;
				}
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
					if (!field.aotFieldDef && _isDheImage &&
						type.aotTypeDef->genericContainerIndex != kGenericContainerIndexInvalid)
					{
						// Matching uses positional variables, but runtime field types
						// must refer to the public Base's actual generic parameters.
						BlobReader logicalReader = _rawImage->GetBlobReaderByRawIndex(data.signature);
						FieldRefSig logicalSignature;
						ReadFieldRefSig(logicalReader, GetGenericContainerFromIl2CppType(type.aotIl2CppType),
							logicalSignature);
						Il2CppType* logicalType = MetadataPool::ShallowCloneIl2CppType(logicalSignature.type);
						logicalType->attrs = data.flags;
						logicalFieldType = logicalType;
					}
					if (field.aotFieldDef)
					{
						_matchedAotFieldTokens.insert(field.aotFieldDef->token);
						_customAttributeTokens[field.aotFieldDef->token] =
							EncodeToken(TableType::FIELD, i);
						SelectCurrentStaticValueField(field, type, nextTypeIndex - 1, i - 1);
					}
				}
				if (!field.aotFieldDef && _interpreterFallbackImage)
				{
					field.aotFieldDef = _interpreterFallbackImage->GetFieldDefinitionFromRawIndex(i - 1);
					const uint32_t rawTypeIndex = static_cast<uint32_t>(
						&type - &typeIntermediateInfos[0]);
					field.declaringIl2CppType =
						_interpreterFallbackImage->GetRawTypeDefinitionType(rawTypeIndex);
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
						_logicalFields[fallbackField] = logicalField;
						_logicalFields[logicalField] = logicalField;
						if ((data.flags & FIELD_ATTRIBUTE_STATIC) == 0)
						{
							if (baseClass->byval_arg.valuetype ||
								(!_isDheImage && type.aotTypeDef->genericContainerIndex != kGenericContainerIndexInvalid) ||
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

	const MethodInfo* SuperSetAOTHomologousImage::ResolveLogicalMethod(const MethodInfo* method)
	{
		if (method && method->is_inflated && method->genericMethod)
		{
			auto definition = _logicalMethods.find(method->genericMethod->methodDefinition);
			if (definition != _logicalMethods.end())
				return il2cpp::metadata::GenericMetadata::Inflate(definition->second, &method->genericMethod->context);
		}
		auto logical = _logicalMethods.find(method);
		return logical == _logicalMethods.end() ? method : logical->second;
	}

	const Il2CppType* SuperSetAOTHomologousImage::GetDheCurrentType(const Il2CppType* type)
	{
		if (!_isDheImage || !_interpreterFallbackImage || !type)
			return nullptr;
		if (type->type == IL2CPP_TYPE_GENERICINST)
		{
			const Il2CppType* definition = GetDheCurrentType(type->data.generic_class->type);
			if (!definition)
				return nullptr;
			Il2CppType current = *type;
			current.data.generic_class = il2cpp::metadata::GenericMetadata::GetGenericClass(
				definition, type->data.generic_class->context.class_inst);
			return MetadataPool::GetPooledIl2CppType(current);
		}
		if (type->type != IL2CPP_TYPE_CLASS && type->type != IL2CPP_TYPE_VALUETYPE)
			return nullptr;
		const Il2CppTypeDefinition* definition = GetUnderlyingTypeDefinition(type);
		if (IsInterpreterType(definition))
			return nullptr;
		auto entry = _aotTypeIndex2TypeDefs.find(il2cpp::vm::GlobalMetadata::GetIndexForTypeDefinition(definition));
		if (entry == _aotTypeIndex2TypeDefs.end())
			return nullptr;
		return _interpreterFallbackImage->GetRawTypeDefinitionType(
			static_cast<uint32_t>(entry->second - _typeDefs.data()));
	}

	bool SuperSetAOTHomologousImage::TryGetDheCurrentInterfaceMethod(const Il2CppClass* klass,
		uint16_t logicalSlot, const MethodInfo*& method)
	{
		const Il2CppClass* definition = klass->generic_class
			? il2cpp::vm::GenericClass::GetTypeDefinition(klass->generic_class) : klass;
		auto entry = _interfaceMethods.find(definition);
		if (entry == _interfaceMethods.end())
			return false;
		if (logicalSlot >= entry->second.size() || !entry->second[logicalSlot])
			il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetMissingMethodException(
				"The Base interface member is not present in the current DHE assembly."));
		method = entry->second[logicalSlot];
		if (klass->generic_class)
			method = il2cpp::metadata::GenericMetadata::Inflate(method, &klass->generic_class->context);
		return true;
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

	const Il2CppType* SuperSetAOTHomologousImage::GetExecutionTypeFromRawTypeDefIndex(uint32_t index)
	{
		return _currentStorageTypeTokens.count(EncodeToken(TableType::TYPEDEF, index + 1))
			? _interpreterFallbackImage->GetRawTypeDefinitionType(index)
			: GetIl2CppTypeFromRawTypeDefIndex(index);
	}

	const Il2CppType* SuperSetAOTHomologousImage::GetDheExecutionType(const Il2CppType* type)
	{
		if (!type || _currentStorageTypeTokens.empty()) return nullptr;
		if (type->type == IL2CPP_TYPE_ARRAY)
		{
			const Il2CppType* currentElement = GetDheExecutionType(type->data.array->etype);
			if (!currentElement)
				currentElement = GetDheCurrentType(type->data.array->etype);
			if (!currentElement)
				return nullptr;
			Il2CppType current = *type;
			current.data.array = const_cast<Il2CppArrayType*>(MetadataPool::GetPooledIl2CppArrayType(
				currentElement, type->data.array->rank));
			return MetadataPool::GetPooledIl2CppType(current);
		}
		// Generic instances carry a Base generic definition plus a class
		// instantiation. Preserve the arguments and replace only the selected
		// physical definition; this avoids routing Current generic fields through
		// the reference sidecar used by ordinary AOT representations.
		if (type->type == IL2CPP_TYPE_GENERICINST)
		{
			const Il2CppType* currentDefinition = GetDheExecutionType(type->data.generic_class->type);
			// The generic definition may be unchanged while one of its value-type
			// arguments moved to Current storage (for example GenericValue<Payload>).
			// Keep the Base definition in that case and rebuild the instantiation
			// with the remapped arguments so its field layout is recomputed.
			if (!currentDefinition) currentDefinition = type->data.generic_class->type;
			const Il2CppGenericInst* baseInst = type->data.generic_class->context.class_inst;
			std::vector<const Il2CppType*> mappedArgs(baseInst->type_argc);
			bool argumentsChanged = false;
			for (uint32_t i = 0; i < baseInst->type_argc; ++i)
			{
				const Il2CppType* argument = baseInst->type_argv[i];
				const Il2CppType* mapped = GetDheExecutionType(argument);
				mappedArgs[i] = mapped ? mapped : argument;
				argumentsChanged |= mappedArgs[i] != argument;
			}
			const Il2CppGenericInst* currentInst = argumentsChanged
				? il2cpp::vm::MetadataCache::GetGenericInst(mappedArgs.data(), baseInst->type_argc)
				: baseInst;
			if (!argumentsChanged && currentDefinition == type->data.generic_class->type)
				return nullptr;
			Il2CppGenericClass* currentGeneric = il2cpp::metadata::GenericMetadata::GetGenericClass(
				currentDefinition, currentInst);
			// Mapping is also used while signatures are being decoded. Keep it
			// independent of class/layout initialization and preserve type flags.
			Il2CppType current = *type;
			current.data.generic_class = currentGeneric;
			return MetadataPool::GetPooledIl2CppType(current);
		}
		// TypeRef decoding supplies a definition; byref/array wrappers are built
		// by the signature reader afterwards.
		if (type->type != IL2CPP_TYPE_CLASS && type->type != IL2CPP_TYPE_VALUETYPE) return nullptr;
		const Il2CppTypeDefinition* definition = GetUnderlyingTypeDefinition(type);
		// A type already decoded from the Current interpreter image is its own
		// execution representation. This matters for arrays whose element type
		// is a Current TypeRef rather than a Base AOT type.
		if (definition && IsInterpreterType(definition)) return type;
		if (!definition) return nullptr;
		auto entry = _aotTypeIndex2TypeDefs.find(il2cpp::vm::GlobalMetadata::GetIndexForTypeDefinition(definition));
		if (entry == _aotTypeIndex2TypeDefs.end()) return nullptr;
		const uint32_t index = static_cast<uint32_t>(entry->second - _typeDefs.data());
		if (!_currentStorageTypeTokens.count(EncodeToken(TableType::TYPEDEF, index + 1))) return nullptr;
		Il2CppType current = *type;
		current.data = _interpreterFallbackImage->GetRawTypeDefinitionType(index)->data;
		return MetadataPool::GetPooledIl2CppType(current);
	}

	bool SuperSetAOTHomologousImage::IsDheField(const FieldInfo* field)
	{
		return field && (_logicalFields.find(field) != _logicalFields.end() ||
			_supplementalFieldLogicalParents.find(field) != _supplementalFieldLogicalParents.end());
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
		auto SelectExecutionType = [this](const Il2CppType* type) -> const Il2CppType*
		{
			// Keep resolution signatures on the logical Base representation. The
			// execution image translates only field owners after method matching;
			// mixing Current types here makes an unchanged Base method signature
			// fail its lookup before the Current execution binding is applied.
			return type;
		};
		TableType tokenType;
		uint32_t rawIndex;
		DecodeResolutionScopeCodedIndex(scope, tokenType, rawIndex);
		switch (tokenType)
		{
		case TableType::MODULE:
		{
			const Il2CppType* retType = GetModuleIl2CppType(rawIndex, typeNamespace, typeName, false);
			return SelectExecutionType(retType ? retType : _defaultIl2CppType);
		}
		case TableType::MODULEREF:
		{
			RaiseNotSupportedException("Image::ReadTypeFromResolutionScope not support ResolutionScore.MODULEREF");
			return nullptr;
		}
		case TableType::ASSEMBLYREF:
		{
			const Il2CppType* refType = GetIl2CppType(rawIndex, typeNamespace, typeName, false);
			return SelectExecutionType(refType ? refType : _defaultIl2CppType);
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
					return SelectExecutionType(GetIl2CppTypeFromTypeDefinition(nextTypeDef));
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

	const std::vector<const MethodInfo*>* SuperSetAOTHomologousImage::GetSupplementalMethods(Il2CppClass* klass)
	{
		auto direct = _supplementalMethods.find(klass);
		if (direct != _supplementalMethods.end())
			return &direct->second;
		if (!_isDheImage || !klass->generic_class)
			return nullptr;
		Il2CppClass* definition = il2cpp::vm::GenericClass::GetTypeDefinition(klass->generic_class);
		auto declared = _supplementalMethods.find(definition);
		if (declared == _supplementalMethods.end())
			return nullptr;

		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		auto cached = _genericSupplementalMethods.find(klass);
		if (cached != _genericSupplementalMethods.end())
			return &cached->second;
		const Il2CppGenericContext* context = il2cpp::vm::GenericClass::GetContext(klass->generic_class);
		std::vector<const MethodInfo*> methods;
		methods.reserve(declared->second.size());
		for (const MethodInfo* method : declared->second)
			methods.push_back(il2cpp::metadata::GenericMetadata::Inflate(method, context));
		// Publish the complete immutable list last. The map's node and the
		// moved vector storage stay stable for Class::GetMethods iterators.
		return &_genericSupplementalMethods.emplace(klass, std::move(methods)).first->second;
	}

	const MethodInfo* SuperSetAOTHomologousImage::GetFirstSupplementalMethod(
		Il2CppClass* klass, void** iter)
	{
		const std::vector<const MethodInfo*>* methods = GetSupplementalMethods(klass);
		if (!methods || methods->empty())
		{
			return nullptr;
		}
		*iter = const_cast<const MethodInfo**>(methods->data());
		return (*methods)[0];
	}

	bool SuperSetAOTHomologousImage::TryGetNextSupplementalMethod(Il2CppClass* klass,
		void** iter, const MethodInfo** method)
	{
		const std::vector<const MethodInfo*>* methods = GetSupplementalMethods(klass);
		if (!methods || methods->empty() || !*iter)
		{
			return false;
		}
		const uintptr_t current = reinterpret_cast<uintptr_t>(*iter);
		const uintptr_t begin = reinterpret_cast<uintptr_t>(methods->data());
		const uintptr_t end = reinterpret_cast<uintptr_t>(
			methods->data() + methods->size());
		if (current < begin || current >= end ||
			(current - begin) % sizeof(const MethodInfo*) != 0)
		{
			return false;
		}
		const size_t nextIndex = (current - begin) / sizeof(const MethodInfo*) + 1;
		if (nextIndex >= methods->size())
		{
			*method = nullptr;
			return true;
		}
		*iter = const_cast<const MethodInfo**>(methods->data() + nextIndex);
		*method = (*methods)[nextIndex];
		return true;
	}

	const MethodInfo* SuperSetAOTHomologousImage::GetCurrentMethodMetadata(const MethodInfo* method)
	{
		const MethodInfo* definition = method->is_inflated && method->genericMethod
			? method->genericMethod->methodDefinition : method;
		auto current = _currentMetadataMethods.find(definition);
		if (current == _currentMetadataMethods.end()) return method;
		return method->is_inflated && method->genericMethod
			? il2cpp::metadata::GenericMetadata::Inflate(current->second, &method->genericMethod->context)
			: current->second;
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
		const std::vector<const MethodInfo*>* methods = GetSupplementalMethods(klass);
		return methods ? methods->size() : 0;
	}

	const std::vector<FieldInfo*>* SuperSetAOTHomologousImage::GetSupplementalFields(Il2CppClass* klass)
	{
		auto direct = _supplementalFields.find(klass);
		if (direct != _supplementalFields.end())
			return &direct->second;
		if (!_isDheImage || !klass->generic_class)
			return nullptr;
		Il2CppClass* definition = il2cpp::vm::GenericClass::GetTypeDefinition(klass->generic_class);
		auto declared = _supplementalFields.find(definition);
		if (declared == _supplementalFields.end())
			return nullptr;

		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		auto cached = _genericSupplementalFields.find(klass);
		if (cached != _genericSupplementalFields.end())
			return &cached->second;
		const Il2CppGenericContext* context = il2cpp::vm::GenericClass::GetContext(klass->generic_class);
		std::vector<FieldInfo*> fields;
		fields.reserve(declared->second.size());
		for (FieldInfo* definitionField : declared->second)
		{
			Il2CppClass* physicalOwner = il2cpp::vm::GenericClass::GetClass(
				il2cpp::metadata::GenericMetadata::GetGenericClass(definitionField->parent, context->class_inst));
			il2cpp::vm::Class::SetupFields(physicalOwner);
			FieldInfo* physical = FindDhePhysicalField(physicalOwner, definitionField);
			if (!physical)
				RaiseMissingFieldException(&physicalOwner->byval_arg, definitionField->name);
			FieldInfo* logical = static_cast<FieldInfo*>(HYBRIDCLR_METADATA_MALLOC(sizeof(FieldInfo)));
			*logical = *physical;
			logical->type = il2cpp::metadata::GenericMetadata::InflateIfNeeded(
				definitionField->type, context, false);
			// Closed generic value types are laid out in the Current value
			// representation and travel inline through interpreter frames. A
			// reference sidecar would manufacture a separate cell and lose the
			// value on copies/arrays. Only reference-type generic owners need the
			// sidecar bridge for old Base object instances.
			if (!klass->byval_arg.valuetype &&
				(logical->type->attrs & FIELD_ATTRIBUTE_STATIC) == 0)
				MetadataModule::RegisterDheSupplementalInstanceField(physical, logical, definitionField);
			fields.push_back(logical);
		}
		// The metadata lock publishes the complete vector and its sidecar aliases.
		return &_genericSupplementalFields.emplace(klass, std::move(fields)).first->second;
	}

	FieldInfo* SuperSetAOTHomologousImage::GetFirstSupplementalField(
		Il2CppClass* klass, void** iter)
	{
		const std::vector<FieldInfo*>* fields = GetSupplementalFields(klass);
		if (!fields || fields->empty())
		{
			return nullptr;
		}
		*iter = const_cast<FieldInfo**>(fields->data());
		return (*fields)[0];
	}

	bool SuperSetAOTHomologousImage::TryGetNextSupplementalField(Il2CppClass* klass,
		void** iter, FieldInfo** field)
	{
		const std::vector<FieldInfo*>* fields = GetSupplementalFields(klass);
		if (!fields || fields->empty() || !*iter)
		{
			return false;
		}
		const uintptr_t current = reinterpret_cast<uintptr_t>(*iter);
		const uintptr_t begin = reinterpret_cast<uintptr_t>(fields->data());
		const uintptr_t end = reinterpret_cast<uintptr_t>(
			fields->data() + fields->size());
		if (current < begin || current >= end ||
			(current - begin) % sizeof(FieldInfo*) != 0)
		{
			return false;
		}
		const size_t nextIndex = (current - begin) / sizeof(FieldInfo*) + 1;
		if (nextIndex >= fields->size())
		{
			*field = nullptr;
			return true;
		}
		*iter = const_cast<FieldInfo**>(fields->data() + nextIndex);
		*field = (*fields)[nextIndex];
		return true;
	}

	size_t SuperSetAOTHomologousImage::GetSupplementalFieldCount(Il2CppClass* klass)
	{
		const std::vector<FieldInfo*>* fields = GetSupplementalFields(klass);
		return fields ? fields->size() : 0;
	}

	bool SuperSetAOTHomologousImage::IsRemovedField(const FieldInfo* field)
	{
		if (_isDheImage && field->parent->generic_class)
			field = FindDhePhysicalField(il2cpp::vm::GenericClass::GetTypeDefinition(
				field->parent->generic_class), field);
		return _removedFields.find(field) != _removedFields.end();
	}

	const FieldInfo* SuperSetAOTHomologousImage::ResolveSupplementalField(const FieldInfo* field)
	{
		auto direct = _logicalFields.find(field);
		if (direct != _logicalFields.end())
			return direct->second;
		Il2CppClass* owner = GetSupplementalFieldLogicalParent(field);
		const std::vector<FieldInfo*>* fields = owner ? GetSupplementalFields(owner) : nullptr;
		if (fields)
			for (FieldInfo* candidate : *fields)
				if (candidate->token == field->token && std::strcmp(candidate->name, field->name) == 0)
					return candidate;
		return field;
	}

	const Il2CppFieldDefinition* SuperSetAOTHomologousImage::ResolveSupplementalFieldDefinition(
		const Il2CppType* type, const char* name, const Il2CppType* fieldType)
	{
		// Resolve against the physical Current class first. This is required
		// while an unchanged Base caller names a field whose Current type has a
		// different layout or field type; signature comparison against the old
		// Base field would reject the valid Current declaration.
		if (_isDheImage && _interpreterFallbackImage && type && name)
		{
			const Il2CppType* executionType = GetDheExecutionType(type);
			if (executionType)
			{
				Il2CppClass* physicalClass = il2cpp::vm::Class::FromIl2CppType(executionType);
				if (physicalClass)
				{
					il2cpp::vm::Class::SetupFields(physicalClass);
					for (uint16_t index = 0; index < physicalClass->field_count; ++index)
					{
						FieldInfo* physicalField = physicalClass->fields + index;
						if (physicalField->name && std::strcmp(physicalField->name, name) == 0)
							return _interpreterFallbackImage->GetFieldDefinitionFromRawIndex(
								DecodeTokenRowIndex(physicalField->token) - 1);
					}
				}
			}
		}
		Il2CppClass* definition = type->type == IL2CPP_TYPE_GENERICINST
			? il2cpp::vm::GenericClass::GetTypeDefinition(type->data.generic_class)
			: il2cpp::vm::Class::FromIl2CppType(type);
		auto fields = _supplementalFields.find(definition);
		if (fields == _supplementalFields.end())
			return nullptr;
		const Il2CppGenericContainer* container = GetGenericContainerFromIl2CppType(type);
		bool currentStorage = false;
		if (definition && definition->image)
		{
			const Il2CppTypeDefinition* baseDefinition = GetUnderlyingTypeDefinition(&definition->byval_arg);
			auto typeEntry = _aotTypeIndex2TypeDefs.find(
				il2cpp::vm::GlobalMetadata::GetIndexForTypeDefinition(baseDefinition));
			if (typeEntry != _aotTypeIndex2TypeDefs.end())
				currentStorage = _currentStorageTypeTokens.count(EncodeToken(TableType::TYPEDEF,
					static_cast<uint32_t>(typeEntry->second - _typeDefs.data()) + 1)) != 0;
		}
		for (FieldInfo* field : fields->second)
			if (std::strcmp(field->name, name) == 0 &&
				(currentStorage || IsMatchSigType(field->type, fieldType, container, nullptr)))
				return _interpreterFallbackImage->GetFieldDefinitionFromRawIndex(DecodeTokenRowIndex(field->token) - 1);
		return nullptr;
	}

	Il2CppClass* SuperSetAOTHomologousImage::GetSupplementalFieldLogicalParent(
		const FieldInfo* field)
	{
		auto logical = _logicalFields.find(field);
		if (_isDheImage && logical != _logicalFields.end())
			field = logical->second;
		auto parent = _supplementalFieldLogicalParents.find(field);
		if (parent != _supplementalFieldLogicalParents.end())
			return parent->second;
		if (!_isDheImage || !field || !field->parent->generic_class)
			return nullptr;
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		Il2CppGenericClass* generic = field->parent->generic_class;
		FieldInfo* definitionField = FindDhePhysicalField(il2cpp::vm::GenericClass::GetTypeDefinition(generic), field);
		auto alias = _logicalFields.find(definitionField);
		if (alias == _logicalFields.end())
			return nullptr;
		Il2CppClass* baseDefinition = _supplementalFieldLogicalParents.at(alias->second);
		return il2cpp::vm::GenericClass::GetClass(il2cpp::metadata::GenericMetadata::GetGenericClass(
			baseDefinition, il2cpp::vm::GenericClass::GetContext(generic)->class_inst));
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
