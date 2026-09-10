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
    private:
        static Il2CppAssembly* Create(const byte* assemblyData, uint64_t length, const byte* rawSymbolStoreBytes, uint64_t rawSymbolStoreLength);
    };
}
}
