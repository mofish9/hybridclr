#pragma once

#include "../CommonDef.h"

#include "InterpreterImage.h"
#include "AOTHomologousImage.h"

namespace hybridclr
{
namespace metadata
{

    class Assembly
    {
    public:
        static void InitializePlaceHolderAssemblies();
#if HYBRIDCLR_ENABLE_AOT_SELECTION
        static const Il2CppAssembly* FindDeferredUnityAssembly(const char* name);
        static const Il2CppImage* GetDeferredUnityImage(const Il2CppImage* image);
        static void BindDeferredUnityImage(const Il2CppAssembly* baseAssembly);
#endif
        static Il2CppAssembly* LoadFromBytes(const void* assemblyData, uint64_t length, const void* rawSymbolStoreBytes, uint64_t rawSymbolStoreLength);
        static LoadImageErrorCode LoadMetadataForAOTAssembly(const void* dllBytes, uint32_t dllSize,
            HomologousImageMode mode, const Il2CppAssembly** targetAssembly = nullptr,
            AOTHomologousImage** targetImage = nullptr, const char* expectedAssemblyName = nullptr,
            const dhe::CurrentImagePlan* currentImagePlan = nullptr,
            bool deferRuntimeInitialization = false);
        static void InitializeDheMetadataBatch(const std::vector<AOTHomologousImage*>& images,
            const std::vector<InterpreterImage*>& interpreterImages = {});
        static LoadImageErrorCode PrepareDheInterpreterAssembly(const void* bytes, uint32_t size,
            InterpreterImage*& image, Il2CppAssembly*& assembly);
        static void RunDheModuleInitializer(Il2CppAssembly* assembly);
        static void RunDheMutableModuleInitializer(AOTHomologousImage* image);
    private:
        static Il2CppAssembly* Create(const byte* assemblyData, uint64_t length, const byte* rawSymbolStoreBytes, uint64_t rawSymbolStoreLength);
    };
}
}
