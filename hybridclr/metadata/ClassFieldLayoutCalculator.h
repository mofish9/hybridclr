#pragma once

#include <unordered_map>
#include <vector>

#include "MetadataUtil.h"

namespace hybridclr
{
namespace metadata
{
	#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
	void RecordMetadataInitStage(uint32_t imageIndex, const char* stage, uint64_t elapsedNanoseconds);
	#endif

	struct FieldLayout
	{
		const Il2CppType* type;
		int32_t offset;
		int32_t size;
		bool isNormalStatic;
		bool isThreadStatic;
	};

	struct ClassLayoutInfo
	{
		const Il2CppType* type;
		std::vector<FieldLayout> fields;
		int32_t instanceSize;
		int32_t actualSize;
		int32_t nativeSize;
		uint32_t staticFieldsSize;
		uint32_t threadStaticFieldsSize;
		uint8_t alignment;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
		uint8_t naturalAlignment;
#endif
		bool blittable;
		bool hasStaticFields;
	};

	struct SizeAndAlignment
	{
		int32_t size;
		int32_t nativeSize;
		uint8_t alignment;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
		uint8_t naturalAlignment;
#endif
	};

	struct FieldLayoutData
	{
		int32_t classSize;
		int32_t actualClassSize;
		int32_t nativeSize;
		
		uint8_t minimumAlignment;
#if !HYBRIDCLR_UNITY_2022_OR_NEW
		uint8_t naturalAlignment;
#endif
	};

	enum class FieldLayoutKind
	{
		Instance,
		Static,
		ThreadStatic,
	};

	class InterpreterImage;

	typedef Il2CppHashMap<const Il2CppType*, ClassLayoutInfo*, il2cpp::metadata::Il2CppTypeHash, il2cpp::metadata::Il2CppTypeEqualityComparer> Il2CppType2ClassLayoutInfoMap;

	class ClassFieldLayoutCalculator
	{
	private:
		InterpreterImage* _image;
		Il2CppType2ClassLayoutInfoMap _classMap;
		// Ordinary types in this image have a stable raw index. Keep a direct
		// pointer table for repeated layout lookups; generic instantiations and
		// cross-image types remain in _classMap because their keys carry context.
		std::vector<ClassLayoutInfo*> _classLayoutByTypeIndex;

	public:
		ClassFieldLayoutCalculator(InterpreterImage* image);

		~ClassFieldLayoutCalculator();
		void FlushInstrumentation();

		ClassLayoutInfo* GetClassLayoutInfo(const Il2CppType* type);

		void CalcClassNotStaticFields(const Il2CppType* type);
		void CalcClassStaticFields(const Il2CppType* type);

		void LayoutFields(int32_t actualParentSize, int32_t parentAlignment, uint8_t packing, std::vector<FieldLayout>& fields, FieldLayoutKind kind, FieldLayoutData& data);
		SizeAndAlignment GetTypeSizeAndAlignment(const Il2CppType* type);
		bool IsBlittable(const Il2CppType* type);

	private:
		bool TryGetLocalTypeIndex(const Il2CppType* type, uint32_t& index) const;
		SizeAndAlignment GetTypeSizeAndAlignmentUncached(const Il2CppType* type);
		bool IsBlittableUncached(const Il2CppType* type);
		bool ShouldCacheType(const Il2CppType* type) const;

		// Layout calculations repeatedly query the same field types while walking
		// nested value types. Keep this cache local to one calculator lifetime so
		// it never outlives the metadata/type graph it describes.
		std::unordered_map<const Il2CppType*, SizeAndAlignment> _sizeAndAlignmentCache;
		std::unordered_map<const Il2CppType*, bool> _blittableCache;
		#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		uint64_t _sizeAndAlignmentCacheHits = 0;
		uint64_t _sizeAndAlignmentCacheMisses = 0;
		uint64_t _blittableCacheHits = 0;
		uint64_t _blittableCacheMisses = 0;
		bool _instrumentationFlushed = false;
		#endif
	};
}
}
