#include "AOTHomologousImage.h"
#include "InterpreterImage.h"

#include <algorithm>
#include "vm/MetadataLock.h"
#include "vm/GlobalMetadata.h"
#include "vm/Class.h"
#include "vm/Image.h"
#include "vm/Exception.h"
#include "vm/MetadataCache.h"
#include "metadata/GenericMetadata.h"

namespace hybridclr
{
namespace metadata
{
	std::vector<AOTHomologousImage*> s_images;
	static thread_local const std::vector<AOTHomologousImage*>* s_preparingImages = nullptr;
	static thread_local const std::vector<InterpreterImage*>* s_preparingInterpreterImages = nullptr;

	AOTHomologousImage::PreparationScope::PreparationScope(
		const std::vector<AOTHomologousImage*>& images, il2cpp::os::FastAutoLock&,
		const std::vector<InterpreterImage*>* interpreterImages)
		: _previous(s_preparingImages), _previousInterpreters(s_preparingInterpreterImages)
	{
		s_preparingImages = &images;
		s_preparingInterpreterImages = interpreterImages;
	}

	AOTHomologousImage::PreparationScope::~PreparationScope()
	{
		s_preparingImages = _previous;
		s_preparingInterpreterImages = _previousInterpreters;
	}

	const Il2CppType* AOTHomologousImage::FindPreparingInterpreterType(const char* assemblyName, const char* namespaze, const char* name)
	{
		if (s_preparingInterpreterImages)
			for (InterpreterImage* image : *s_preparingInterpreterImages)
				if (std::strcmp(image->GetIl2CppImage()->assembly->aname.name, assemblyName) == 0)
					for (uint32_t index = 0; index < image->GetTypeDefinitionCount(); ++index)
					{
						const Il2CppTypeDefinition* type = image->GetTypeFromRawIndex(index);
						if (type->declaringTypeIndex == kTypeDefinitionIndexInvalid &&
							std::strcmp(il2cpp::vm::GlobalMetadata::GetStringFromIndex(type->namespaceIndex), namespaze) == 0 &&
							std::strcmp(il2cpp::vm::GlobalMetadata::GetStringFromIndex(type->nameIndex), name) == 0)
							return image->GetRawTypeDefinitionType(index);
					}
		return nullptr;
	}

	AOTHomologousImage* AOTHomologousImage::FindPreparingImageByAssembly(const Il2CppAssembly* ass)
	{
		if (s_preparingImages)
			for (AOTHomologousImage* image : *s_preparingImages)
				if (image->_targetAssembly == ass) return image;
		return nullptr;
	}


	AOTHomologousImage* AOTHomologousImage::FindImageByAssembly(const Il2CppAssembly* ass)
	{
		il2cpp::os::FastAutoLock lock(&il2cpp::vm::g_MetadataLock);
		return FindImageByAssemblyLocked(ass, lock);
	}

	void AOTHomologousImage::RegisterLocked(AOTHomologousImage* image, il2cpp::os::FastAutoLock& lock)
	{
		IL2CPP_ASSERT(FindImageByAssemblyLocked(image->_targetAssembly, lock) == nullptr);
		s_images.push_back(image);
	}

	bool AOTHomologousImage::UnregisterLocked(AOTHomologousImage* image, il2cpp::os::FastAutoLock&)
	{
		auto it = std::find(s_images.begin(), s_images.end(), image);
		if (it == s_images.end())
		{
			return false;
		}
		s_images.erase(it);
		return true;
	}

	AOTHomologousImage* AOTHomologousImage::FindImageByAssemblyLocked(const Il2CppAssembly* ass, il2cpp::os::FastAutoLock& lock)
	{
		if (AOTHomologousImage* preparing = FindPreparingImageByAssembly(ass)) return preparing;
		for (AOTHomologousImage* image : s_images)
		{
			if (image->_targetAssembly == ass)
			{
				return image;
			}
		}
		return nullptr;
	}

	LoadImageErrorCode AOTHomologousImage::Load(const byte* imageData, size_t length)
	{
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

		TbAssembly data = _rawImage->ReadAssembly(1);
		const char* assName = _rawImage->GetStringFromRawIndex(data.name);
		const Il2CppAssembly* aotAss = GetLoadedAssembly(assName);
		// FIXME. not free memory.
		if (!aotAss)
		{
			return LoadImageErrorCode::AOT_ASSEMBLY_NOT_FIND;
		}
		if (hybridclr::metadata::IsInterpreterImage(aotAss->image))
		{
			return LoadImageErrorCode::HOMOLOGOUS_ONLY_SUPPORT_AOT_ASSEMBLY;
		}
		_targetAssembly = aotAss;

		return LoadImageErrorCode::OK;
	}

	const Il2CppType* AOTHomologousImage::GetModuleIl2CppType(uint32_t moduleRowIndex, uint32_t typeNamespace, uint32_t typeName, bool raiseExceptionIfNotFound)
	{
		IL2CPP_ASSERT(moduleRowIndex == 1);
		const char* typeNameStr = _rawImage->GetStringFromRawIndex(typeName);
		const char* typeNamespaceStr = _rawImage->GetStringFromRawIndex(typeNamespace);

		const Il2CppImage* aotImage = il2cpp::vm::Assembly::GetImage(_targetAssembly);
		Il2CppClass* klass = il2cpp::vm::Class::FromName(aotImage, typeNamespaceStr, typeNameStr);
		if (klass)
		{
			return &klass->byval_arg;
		}
		if (!raiseExceptionIfNotFound)
		{
			return nullptr;
		}
		il2cpp::vm::Exception::Raise(il2cpp::vm::Exception::GetTypeLoadException(
			CStringToStringView(typeNamespaceStr),
			CStringToStringView(typeNameStr),
			CStringToStringView(aotImage->nameNoExt)));
		return nullptr;
	}
}
}
