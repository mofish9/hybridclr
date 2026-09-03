#pragma once

#include <stdint.h>
#include "CommonDef.h"

namespace hybridclr
{
	class RuntimeApi
	{
	public:
		static void RegisterInternalCalls();

		static int32_t LoadMetadataForAOTAssembly(Il2CppArray* dllData, int32_t mode);
		static int32_t LoadDifferentialHybridAssemblyWithMetaVersion(Il2CppArray* dllData, Il2CppArray* baseMvData, Il2CppArray* currentMvData);
		static int32_t LoadDifferentialHybridAssembliesWithMetaVersion(Il2CppArray* dllData, Il2CppArray* baseMvData, Il2CppArray* currentMvData);
		static int32_t IsDifferentialMethodChanged(Il2CppReflectionMethod* method);
		static int32_t GetDifferentialInterpreterEntryCount();
		static int32_t GetDifferentialAotBridgeCallCount();
		static int32_t GetDifferentialAotEntryCount();
		static void ResetDifferentialDispatchCounters();

		static int32_t GetRuntimeOption(int32_t optionId);
		static void SetRuntimeOption(int32_t optionId, int32_t value);

		static int32_t PrewarmMethod(Il2CppReflectionMethod* method);
		static int32_t PrewarmMethodBase(Il2CppReflectionMethod* method);
		static int32_t PrewarmMethodBaseBatch(Il2CppArray* methods, int32_t count);
		static int32_t PrewarmMethodBaseBatchResultMask(Il2CppArray* methods, int32_t count);
		static int32_t PrewarmMethodToken(Il2CppReflectionType* declaringType, int32_t metadataToken);
		static int32_t PrewarmMethodTokenBatch(Il2CppArray* declaringTypes, Il2CppArray* metadataTokens, int32_t count);
		static int32_t PrewarmMethodTokenBatchResultMask(Il2CppArray* declaringTypes, Il2CppArray* metadataTokens, int32_t count);
		static int32_t PreJitClass(Il2CppReflectionType* type);
		static int32_t PreJitMethod(Il2CppReflectionMethod* method);
		static int32_t PrewarmClass(Il2CppReflectionType* type);
		static int32_t PrewarmClassBatch(Il2CppArray* types, int32_t count);
		static int32_t PrewarmClassBatchResultMask(Il2CppArray* types, int32_t count);
	};
}
