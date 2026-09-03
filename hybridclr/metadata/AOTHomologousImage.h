#pragma once
#include "Image.h"

namespace hybridclr
{
namespace metadata
{
	struct AOTFieldData
	{
		uint32_t typeDefIndex; // rowIndex - 1
		const Il2CppFieldDefinition* fieldDef;
	};

	enum class HomologousImageMode
	{
		CONSISTENT,
		SUPERSET,
	};

	class AOTHomologousImage : public Image
	{
	public:

		static AOTHomologousImage* FindImageByAssembly(const Il2CppAssembly* ass);
		static AOTHomologousImage* FindImageByAssemblyLocked(const Il2CppAssembly* ass, il2cpp::os::FastAutoLock& lock);
		static void RegisterLocked(AOTHomologousImage* image, il2cpp::os::FastAutoLock& lock);
		// Remove a failed registration while retaining the image allocation.
		// Method-body caches may still refer to its preparation epoch.
		static bool UnregisterLocked(AOTHomologousImage* image, il2cpp::os::FastAutoLock& lock);

		AOTHomologousImage() : _targetAssembly(nullptr) { }

		const Il2CppAssembly* GetTargetAssembly() const
		{
			return _targetAssembly;
		}

		void SetTargetAssembly(const Il2CppAssembly* targetAssembly)
		{
			_targetAssembly = targetAssembly;
		}

		LoadImageErrorCode Load(const byte* imageData, size_t length);

		virtual Il2CppClass* FindSupplementalType(const char* namespaze, const char* name)
		{
			return nullptr;
		}

		virtual void GetSupplementalTypes(std::vector<const Il2CppClass*>& types)
		{
		}

		virtual Il2CppClass* GetFirstSupplementalNestedType(Il2CppClass* klass, void** iter)
		{
			return nullptr;
		}

		virtual bool TryGetNextSupplementalNestedType(Il2CppClass* klass, void** iter,
			Il2CppClass** nestedType)
		{
			return false;
		}

		virtual const MethodInfo* GetFirstSupplementalMethod(Il2CppClass* klass, void** iter)
		{
			return nullptr;
		}

		virtual bool TryGetNextSupplementalMethod(Il2CppClass* klass, void** iter,
			const MethodInfo** method)
		{
			return false;
		}

			virtual Image* GetSupplementalMethodImage(const MethodInfo* method)
			{
				return nullptr;
			}
			virtual Image* GetMethodResolveImage(const MethodInfo* method)
			{
				return nullptr;
			}

		virtual size_t GetSupplementalMethodCount(Il2CppClass* klass)
		{
			return 0;
		}

		virtual FieldInfo* GetFirstSupplementalField(Il2CppClass* klass, void** iter)
		{
			return nullptr;
		}

		virtual bool TryGetNextSupplementalField(Il2CppClass* klass, void** iter,
			FieldInfo** field)
		{
			return false;
		}

		virtual size_t GetSupplementalFieldCount(Il2CppClass* klass)
		{
			return 0;
		}

		virtual bool IsRemovedField(const FieldInfo* field)
		{
			return false;
		}

		virtual Il2CppClass* GetSupplementalFieldLogicalParent(const FieldInfo* field)
		{
			return nullptr;
		}

		virtual bool TryGetCustomAttributeSource(uint32_t token,
			const Il2CppImage*& sourceImage, uint32_t& sourceToken)
		{
			return false;
		}

		virtual bool HasLogicalPropertyView(Il2CppClass* klass)
		{
			return false;
		}

		virtual const PropertyInfo* GetFirstLogicalProperty(Il2CppClass* klass, void** iter)
		{
			return nullptr;
		}

		virtual bool TryGetNextLogicalProperty(Il2CppClass* klass, void** iter,
			const PropertyInfo** property)
		{
			return false;
		}

		virtual size_t GetLogicalPropertyCount(Il2CppClass* klass)
		{
			return 0;
		}

		virtual bool HasLogicalEventView(Il2CppClass* klass)
		{
			return false;
		}

		virtual const EventInfo* GetFirstLogicalEvent(Il2CppClass* klass, void** iter)
		{
			return nullptr;
		}

		virtual bool TryGetNextLogicalEvent(Il2CppClass* klass, void** iter,
			const EventInfo** eventInfo)
		{
			return false;
		}

		virtual size_t GetLogicalEventCount(Il2CppClass* klass)
		{
			return 0;
		}

		const Il2CppType* GetModuleIl2CppType(uint32_t moduleRowIndex, uint32_t typeNamespace, uint32_t typeName, bool raiseExceptionIfNotFound) override;
	protected:
		const Il2CppAssembly* _targetAssembly;
	};
}
}
