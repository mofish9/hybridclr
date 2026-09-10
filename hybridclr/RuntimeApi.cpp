#include "RuntimeApi.h"

#include <cstring>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(__has_include)
#if __has_include("lab/InstrumentationConfig.h")
#include "lab/InstrumentationConfig.h"
#endif
#endif

#include "codegen/il2cpp-codegen.h"
#include "vm/InternalCalls.h"
#include "vm/Array.h"
#include "vm/Exception.h"
#include "vm/Class.h"
#include "vm/Assembly.h"
#include "vm/MetadataLock.h"
#include "vm/MetadataCache.h"
#include "vm/Reflection.h"

#include "metadata/MetadataModule.h"
#include "metadata/MetadataUtil.h"
#include "metadata/Assembly.h"
#include "DheRuntime.h"
#include "metadata/AOTHomologousImage.h"
#include "metadata/RawImage.h"
#include "interpreter/InterpreterModule.h"
#include "interpreter/InterpreterProfile.h"
#include "RuntimeConfig.h"

#if defined(HYBRIDCLR_LAB_INSTRUMENTED) || defined(HYBRIDCLR_LAB_FGS_TESTS)
#include <atomic>
#endif

#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
#include <algorithm>
#include <array>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "interpreter/Instruction.h"
#include "vm/String.h"
#endif
#include <vector>
namespace hybridclr
{
	namespace
	{
		using InterpreterPeerHashes = std::vector<std::pair<std::string, dhe::Sha256Digest>>;
		struct PendingDheImage
		{
			metadata::AOTHomologousImage* image;
			dhe::Sha256Digest currentAssemblyHash;
			std::shared_ptr<const dhe::CurrentImagePlan> executionPlan;
			std::vector<const Il2CppAssembly*> batchAssemblies;
			bool metadataReady = false;
			InterpreterPeerHashes interpreterPeers;
		};
		struct PendingInterpreterImage
		{
			metadata::InterpreterImage* image = nullptr;
			Il2CppAssembly* assembly = nullptr;
			dhe::Sha256Digest hash{};
			std::vector<const Il2CppAssembly*> batchAssemblies;
			InterpreterPeerHashes interpreterPeers;
			bool metadataReady = false;
		};
		struct InterpreterLoadPayload
		{
			const void* bytes = nullptr;
			uint32_t size = 0;
			std::string name;
			dhe::Sha256Digest hash{};
		};

		// DHE images are one-shot process metadata. If MV registration fails
		// after the hidden interpreter image has been initialized, deleting it
		// is unsafe because metadata caches may already reference it. Retain one
		// pending image per Base assembly and reuse it for a corrected MV retry.
		std::mutex s_dheLoadMutex;
		std::unordered_map<const Il2CppAssembly*, PendingDheImage> s_pendingDheImages;
		std::unordered_map<std::string, PendingInterpreterImage> s_pendingInterpreterImages;

		struct DheLoadPayload
		{
			const void* dllData = nullptr;
			uint32_t dllSize = 0;
			dhe::Sha256Digest currentAssemblyHash{};
			dhe::MetaVersionData baseMetaVersion;
			dhe::MetaVersionData currentMetaVersion;
			const Il2CppAssembly* baseAssembly = nullptr;
			metadata::AOTHomologousImage* currentImage = nullptr;
			std::shared_ptr<const dhe::CurrentImagePlan> executionPlan;
		};

		int32_t ParseDheLoadPayload(Il2CppArray* dllBytes, Il2CppArray* baseMvBytes,
			Il2CppArray* currentMvBytes, DheLoadPayload& payload)
		{
			if (!dllBytes || !baseMvBytes || !currentMvBytes)
			{
				return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
			}
			if (!dhe::ParseMetaVersion(il2cpp::vm::Array::GetFirstElementAddress(baseMvBytes),
					il2cpp::vm::Array::GetByteLength(baseMvBytes), payload.baseMetaVersion) ||
				!dhe::ParseMetaVersion(il2cpp::vm::Array::GetFirstElementAddress(currentMvBytes),
					il2cpp::vm::Array::GetByteLength(currentMvBytes), payload.currentMetaVersion) ||
				(payload.baseMetaVersion.flags & dhe::kMetaVersionStrictCompatibilityFlag) == 0 ||
				(payload.currentMetaVersion.flags & dhe::kMetaVersionStrictCompatibilityFlag) == 0 ||
				payload.baseMetaVersion.assemblyName != payload.currentMetaVersion.assemblyName)
			{
				return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
			}

			payload.dllData = il2cpp::vm::Array::GetFirstElementAddress(dllBytes);
			payload.dllSize = il2cpp::vm::Array::GetByteLength(dllBytes);
			if (!dhe::ComputeSha256(payload.dllData, payload.dllSize,
					payload.currentAssemblyHash) ||
				payload.currentAssemblyHash != payload.currentMetaVersion.assemblyHash)
			{
				return (int32_t)metadata::LoadImageErrorCode::DHE_MV_CURRENT_HASH_MISMATCH;
			}
			return (int32_t)metadata::LoadImageErrorCode::OK;
		}

		int32_t LoadDhePayloads(std::vector<DheLoadPayload>& payloads,
			const std::vector<InterpreterLoadPayload>& interpreterPayloads = {})
		{
			if (payloads.empty())
			{
				return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
			}

			std::unique_lock<std::mutex> loadLock(s_dheLoadMutex);
			std::unordered_set<const Il2CppAssembly*> uniqueAssemblies;
			std::unordered_set<std::string> uniqueNames;
			InterpreterPeerHashes interpreterPeers;
			for (const auto& input : interpreterPayloads)
			{
				if (!uniqueNames.insert(input.name).second || il2cpp::vm::MetadataCache::GetAssemblyByName(input.name.c_str()))
					return (int32_t)metadata::LoadImageErrorCode::HOMOLOGOUS_ASSEMBLY_HAS_BEEN_LOADED;
				interpreterPeers.push_back({ input.name, input.hash });
			}
			std::sort(interpreterPeers.begin(), interpreterPeers.end());
			for (DheLoadPayload& payload : payloads)
			{
				payload.baseAssembly = il2cpp::vm::MetadataCache::GetAssemblyByName(
					payload.baseMetaVersion.assemblyName.c_str());
				if (!payload.baseAssembly || !payload.baseAssembly->image ||
					metadata::IsInterpreterImage(payload.baseAssembly->image))
				{
					return (int32_t)metadata::LoadImageErrorCode::AOT_ASSEMBLY_NOT_FIND;
				}
				if (!uniqueAssemblies.insert(payload.baseAssembly).second ||
					!uniqueNames.insert(payload.baseMetaVersion.assemblyName).second ||
					dhe::IsDheAssembly(payload.baseAssembly))
				{
					return (int32_t)metadata::LoadImageErrorCode::
						HOMOLOGOUS_ASSEMBLY_HAS_BEEN_LOADED;
				}
				auto pending = s_pendingDheImages.find(payload.baseAssembly);
				if (pending != s_pendingDheImages.end() &&
					(pending->second.currentAssemblyHash != payload.currentAssemblyHash ||
					 bool(pending->second.executionPlan) != bool(payload.executionPlan) ||
					 (payload.executionPlan && !(*pending->second.executionPlan == *payload.executionPlan))))
				{
					return (int32_t)metadata::LoadImageErrorCode::
						HOMOLOGOUS_ASSEMBLY_HAS_BEEN_LOADED;
				}
			}

			std::vector<const Il2CppAssembly*> batchAssemblies;
			for (const DheLoadPayload& payload : payloads) batchAssemblies.push_back(payload.baseAssembly);
			for (const DheLoadPayload& payload : payloads)
			{
				auto pending = s_pendingDheImages.find(payload.baseAssembly);
				if (pending == s_pendingDheImages.end()) continue;
				if (pending->second.interpreterPeers != interpreterPeers)
					return (int32_t)metadata::LoadImageErrorCode::HOMOLOGOUS_ASSEMBLY_HAS_BEEN_LOADED;
				if (!pending->second.metadataReady)
					return (int32_t)metadata::LoadImageErrorCode::DHE_MV_REGISTRATION_FAILED;
				if (pending->second.batchAssemblies.size() != batchAssemblies.size())
					return (int32_t)metadata::LoadImageErrorCode::HOMOLOGOUS_ASSEMBLY_HAS_BEEN_LOADED;
				for (const Il2CppAssembly* peer : pending->second.batchAssemblies)
					if (!uniqueAssemblies.count(peer))
						return (int32_t)metadata::LoadImageErrorCode::HOMOLOGOUS_ASSEMBLY_HAS_BEEN_LOADED;
			}
			for (const auto& input : interpreterPayloads)
			{
				auto pending = s_pendingInterpreterImages.find(input.name);
				if (pending == s_pendingInterpreterImages.end()) continue;
				const PendingInterpreterImage& entry = pending->second;
				if (entry.hash != input.hash || entry.interpreterPeers != interpreterPeers ||
					entry.batchAssemblies.size() != batchAssemblies.size())
					return (int32_t)metadata::LoadImageErrorCode::HOMOLOGOUS_ASSEMBLY_HAS_BEEN_LOADED;
				for (const Il2CppAssembly* peer : entry.batchAssemblies)
					if (!uniqueAssemblies.count(peer)) return (int32_t)metadata::LoadImageErrorCode::HOMOLOGOUS_ASSEMBLY_HAS_BEEN_LOADED;
				if (!entry.metadataReady) return (int32_t)metadata::LoadImageErrorCode::DHE_MV_REGISTRATION_FAILED;
			}
			std::vector<metadata::AOTHomologousImage*> newImages;
			newImages.reserve(payloads.size());
			for (DheLoadPayload& payload : payloads)
			{
				auto pending = s_pendingDheImages.find(payload.baseAssembly);
				if (pending != s_pendingDheImages.end())
				{
					payload.currentImage = pending->second.image;
				}
				else
				{
					const Il2CppAssembly* loadedAssembly = nullptr;
					metadata::LoadImageErrorCode loadError =
						metadata::Assembly::LoadMetadataForAOTAssembly(
							payload.dllData, payload.dllSize,
							metadata::HomologousImageMode::SUPERSET,
							&loadedAssembly, &payload.currentImage,
							payload.baseMetaVersion.assemblyName.c_str(), payload.executionPlan.get(), true);
					if (loadError != metadata::LoadImageErrorCode::OK ||
						loadedAssembly != payload.baseAssembly || !payload.currentImage)
					{
						return (int32_t)(loadError == metadata::LoadImageErrorCode::OK
							? metadata::LoadImageErrorCode::DHE_MV_REGISTRATION_FAILED
							: loadError);
					}
					// Retain allocations before any signature can enter a shared cache.
					// A metadata initialization exception leaves this batch non-retryable;
					// an MV rejection after initialization can reuse the exact graph.
					s_pendingDheImages.emplace(payload.baseAssembly, PendingDheImage{
						payload.currentImage, payload.currentAssemblyHash, payload.executionPlan,
						batchAssemblies, false, interpreterPeers });
					newImages.push_back(payload.currentImage);
				}
			}
			std::vector<metadata::InterpreterImage*> newInterpreterImages;
			std::vector<Il2CppAssembly*> interpreterAssemblies;
			for (const auto& input : interpreterPayloads)
			{
				auto pending = s_pendingInterpreterImages.find(input.name);
				if (pending == s_pendingInterpreterImages.end())
				{
					PendingInterpreterImage entry;
					entry.hash = input.hash; entry.batchAssemblies = batchAssemblies; entry.interpreterPeers = interpreterPeers;
					auto error = metadata::Assembly::PrepareDheInterpreterAssembly(input.bytes, input.size, entry.image, entry.assembly);
					if (error != metadata::LoadImageErrorCode::OK) return (int32_t)error;
					pending = s_pendingInterpreterImages.emplace(input.name, std::move(entry)).first;
					newInterpreterImages.push_back(pending->second.image);
				}
				interpreterAssemblies.push_back(pending->second.assembly);
			}
			metadata::Assembly::InitializeDheMetadataBatch(newImages, newInterpreterImages);
			for (const DheLoadPayload& payload : payloads)
				s_pendingDheImages.at(payload.baseAssembly).metadataReady = true;
			for (const auto& input : interpreterPayloads) s_pendingInterpreterImages.at(input.name).metadataReady = true;

			std::vector<dhe::MetaVersionRegistration> registrations;
			registrations.reserve(payloads.size());
			bool executionPlansReady = true;
			for (DheLoadPayload& payload : payloads)
			{
				registrations.push_back({ payload.baseAssembly,
					&payload.baseMetaVersion, &payload.currentMetaVersion });
				executionPlansReady = payload.currentImage->AppendDheCurrentExecutions(registrations.back()) && executionPlansReady;
			}
			if (!executionPlansReady || !dhe::PrepareAndRegisterMetaVersions(registrations, interpreterAssemblies))
			{
				return (int32_t)metadata::LoadImageErrorCode::DHE_MV_REGISTRATION_FAILED;
			}
			for (DheLoadPayload& payload : payloads)
			{
				s_pendingDheImages.erase(payload.baseAssembly);
			}
			for (const auto& input : interpreterPayloads) s_pendingInterpreterImages.erase(input.name);
			loadLock.unlock();
			// Module code may recursively load assemblies or start other threads.
			// It runs only after the entire graph is committed and load locks exit.
			for (Il2CppAssembly* assembly : interpreterAssemblies) metadata::Assembly::RunDheModuleInitializer(assembly);
			return (int32_t)metadata::LoadImageErrorCode::OK;
		}

		// Parse every MV and selection before starting the image transaction.
		// The resource compiler remains responsible for complete layout/ABI
		// coverage; the native side checks token, identity and member bindings.
		int32_t LoadDhePayloadsWithExecutionPlanAndSources(Il2CppArray* dllBytes, Il2CppArray* baseMvBytes,
			Il2CppArray* currentMvBytes, Il2CppArray* typeSelections, Il2CppArray* methodSelections,
			Il2CppArray* sourceKinds, Il2CppArray* excludedTypeSelections, Il2CppArray* genericContextSelections = nullptr,
			Il2CppArray* interpreterDlls = nullptr)
		{
			if (!dllBytes || !baseMvBytes || !currentMvBytes || !typeSelections || !methodSelections)
				return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
			const uint32_t count = il2cpp::vm::Array::GetLength(dllBytes);
			if (!count || il2cpp::vm::Array::GetLength(baseMvBytes) != count ||
				il2cpp::vm::Array::GetLength(currentMvBytes) != count ||
				il2cpp::vm::Array::GetLength(typeSelections) != count ||
				il2cpp::vm::Array::GetLength(methodSelections) != count)
				return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
			if ((sourceKinds == nullptr) != (excludedTypeSelections == nullptr) ||
				(sourceKinds && il2cpp::vm::Array::GetLength(sourceKinds) != count) ||
				(excludedTypeSelections && il2cpp::vm::Array::GetLength(excludedTypeSelections) != count) ||
				(genericContextSelections && (!sourceKinds || il2cpp::vm::Array::GetLength(genericContextSelections) != count)))
				return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
			auto elements = [](Il2CppArray* array) {
				return reinterpret_cast<Il2CppArray**>(il2cpp::vm::Array::GetFirstElementAddress(array));
			};
			std::vector<DheLoadPayload> payloads(count);
			for (uint32_t index = 0; index < count; ++index)
			{
				DheLoadPayload& payload = payloads[index];
				int32_t error = ParseDheLoadPayload(elements(dllBytes)[index], elements(baseMvBytes)[index],
					elements(currentMvBytes)[index], payload);
				if (error != (int32_t)metadata::LoadImageErrorCode::OK) return error;
				Il2CppArray* types = elements(typeSelections)[index];
				Il2CppArray* methods = elements(methodSelections)[index];
				// Preserve the existing no-execution-plan path for differential
				// peers whose layouts do not require explicit selections.
				if (!types && !methods && interpreterDlls && sourceKinds &&
					reinterpret_cast<const int32_t*>(il2cpp::vm::Array::GetFirstElementAddress(sourceKinds))[index] == 0 &&
					elements(excludedTypeSelections)[index] && !il2cpp::vm::Array::GetLength(elements(excludedTypeSelections)[index]) &&
					genericContextSelections && elements(genericContextSelections)[index] && !il2cpp::vm::Array::GetLength(elements(genericContextSelections)[index]))
					continue;
				if (!types || !methods) return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
				const uint32_t* typeTokens = reinterpret_cast<const uint32_t*>(il2cpp::vm::Array::GetFirstElementAddress(types));
				const uint32_t* methodTokens = reinterpret_cast<const uint32_t*>(il2cpp::vm::Array::GetFirstElementAddress(methods));
				auto plan = std::make_shared<dhe::CurrentImagePlan>();
				dhe::CurrentImageSource source;
				if (genericContextSelections)
				{
					Il2CppArray* conditional = elements(genericContextSelections)[index];
					if (!conditional) return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
					const uint32_t* values = reinterpret_cast<const uint32_t*>(il2cpp::vm::Array::GetFirstElementAddress(conditional));
					source.genericContextMethodTokens.assign(values, values + il2cpp::vm::Array::GetLength(conditional));
				}
				if (sourceKinds)
				{
					const int32_t* kinds = reinterpret_cast<const int32_t*>(il2cpp::vm::Array::GetFirstElementAddress(sourceKinds));
					if (kinds[index] != 0 && kinds[index] != 1)
						return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
					source.kind = static_cast<dhe::CurrentImageSourceKind>(kinds[index]);
					if (source.kind == dhe::CurrentImageSourceKind::FrozenBaseAot)
					{
						source.baseSourceHash = payload.baseMetaVersion.assemblyHash;
						Il2CppArray* excluded = reinterpret_cast<Il2CppArray**>(il2cpp::vm::Array::GetFirstElementAddress(excludedTypeSelections))[index];
						if (!excluded) return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
						const uint32_t* values = reinterpret_cast<const uint32_t*>(il2cpp::vm::Array::GetFirstElementAddress(excluded));
						source.excludedBaseTypeTokens.assign(values, values + il2cpp::vm::Array::GetLength(excluded));
					}
				}
				if (!dhe::BuildCurrentImagePlan(payload.baseMetaVersion, payload.currentMetaVersion,
					std::vector<uint32_t>(typeTokens, typeTokens + il2cpp::vm::Array::GetLength(types)),
					std::vector<uint32_t>(methodTokens, methodTokens + il2cpp::vm::Array::GetLength(methods)), *plan, source))
					return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
				payload.executionPlan = plan;
			}
			std::vector<InterpreterLoadPayload> interpreterPayloads;
			if (interpreterDlls)
				for (uint32_t index = 0; index < il2cpp::vm::Array::GetLength(interpreterDlls); ++index)
				{
					Il2CppArray* dll = elements(interpreterDlls)[index];
					if (!dll) return (int32_t)metadata::LoadImageErrorCode::BAD_IMAGE;
					InterpreterLoadPayload input;
					input.bytes = il2cpp::vm::Array::GetFirstElementAddress(dll);
					input.size = il2cpp::vm::Array::GetByteLength(dll);
					metadata::RawImage raw;
					// RawImage owns and frees its input. The managed byte[] remains
					// borrowed by the batch and must never be released by this parser.
					auto error = raw.Load(CopyBytes(input.bytes, input.size), input.size);
					if (error != metadata::LoadImageErrorCode::OK || raw.GetTable(metadata::TableType::ASSEMBLY).rowNum != 1)
						return (int32_t)metadata::LoadImageErrorCode::BAD_IMAGE;
					input.name = raw.GetStringFromRawIndex(raw.ReadAssembly(1).name);
					if (input.name.empty() || !dhe::ComputeSha256(input.bytes, input.size, input.hash))
						return (int32_t)metadata::LoadImageErrorCode::BAD_IMAGE;
					interpreterPayloads.push_back(std::move(input));
				}
			return LoadDhePayloads(payloads, interpreterPayloads);
		}

		int32_t LoadDhePayloadsWithExecutionPlan(Il2CppArray* dllBytes, Il2CppArray* baseMvBytes,
			Il2CppArray* currentMvBytes, Il2CppArray* typeSelections, Il2CppArray* methodSelections)
		{
			return LoadDhePayloadsWithExecutionPlanAndSources(dllBytes, baseMvBytes, currentMvBytes,
				typeSelections, methodSelections, nullptr, nullptr);
		}

		Il2CppReflectionMethod* ResolveDheCurrentStorageProbe(Il2CppReflectionMethod* method)
		{
			if (!method || !method->method || method->method->parameters_count ||
				!(method->method->flags & METHOD_ATTRIBUTE_STATIC))
				return nullptr;
			const MethodInfo* current = dhe::ResolveInterpreterMethod(method->method);
			return current ? il2cpp::vm::Reflection::GetMethodObject(current, current->klass) : nullptr;
		}
	}

#if defined(HYBRIDCLR_LAB_FGS_TESTS)

	namespace interpreter
	{
		namespace
		{
			std::atomic<int64_t> s_fullGenericSharingDispatchCount{ 0 };
			std::atomic<int64_t> s_fullGenericSharingInterpreterInvokerCount{ 0 };
		}

		void FullGenericSharingDiagnostics::Reset()
		{
			s_fullGenericSharingDispatchCount.store(0, std::memory_order_relaxed);
			s_fullGenericSharingInterpreterInvokerCount.store(0, std::memory_order_relaxed);
		}

		void FullGenericSharingDiagnostics::RecordDispatch()
		{
			s_fullGenericSharingDispatchCount.fetch_add(1, std::memory_order_relaxed);
		}

		void FullGenericSharingDiagnostics::RecordInterpreterInvoker()
		{
			s_fullGenericSharingInterpreterInvokerCount.fetch_add(1, std::memory_order_relaxed);
		}

		int64_t FullGenericSharingDiagnostics::GetDispatchCount()
		{
			return s_fullGenericSharingDispatchCount.load(std::memory_order_relaxed);
		}

		int64_t FullGenericSharingDiagnostics::GetInterpreterInvokerCount()
		{
			return s_fullGenericSharingInterpreterInvokerCount.load(std::memory_order_relaxed);
		}
	}

#endif

#if defined(HYBRIDCLR_LAB_INSTRUMENTED)

	namespace interpreter
	{
		namespace
		{
			constexpr uint16_t kOpcodeCount = static_cast<uint16_t>(HiOpcodeEnum::LdcVarConst_4_Add_i4_Ret_4) + 1;
			constexpr uint16_t kNoOpcode = UINT16_MAX;

			struct ProfileData
			{
				std::array<uint64_t, kOpcodeCount> opcodeCounts{};
				std::unordered_map<uint32_t, uint64_t> transitionCounts;
				uint64_t dispatchCount = 0;
				uint64_t interpreterEntryCount = 0;
				uint64_t transformCount = 0;
				uint64_t transformNanoseconds = 0;
			};

			struct ThreadProfile;

			struct ProfileRegistry
			{
				std::mutex pauseMutex;
				std::mutex profilesMutex;
				std::atomic<bool> recording{ true };
				std::vector<ThreadProfile*> liveProfiles;
				ProfileData retiredData;
			};

			ProfileRegistry& GetProfileRegistry()
			{
				static ProfileRegistry* registry = new ProfileRegistry();
				return *registry;
			}

			void MergeProfileData(ProfileData& destination, const ProfileData& source)
			{
				for (uint16_t i = 0; i < kOpcodeCount; i++)
				{
					destination.opcodeCounts[i] += source.opcodeCounts[i];
				}
				for (const auto& transition : source.transitionCounts)
				{
					destination.transitionCounts[transition.first] += transition.second;
				}
				destination.dispatchCount += source.dispatchCount;
				destination.interpreterEntryCount += source.interpreterEntryCount;
				destination.transformCount += source.transformCount;
				destination.transformNanoseconds += source.transformNanoseconds;
			}

			void ClearProfileData(ProfileData& data)
			{
				data.opcodeCounts.fill(0);
				data.transitionCounts.clear();
				data.dispatchCount = 0;
				data.interpreterEntryCount = 0;
				data.transformCount = 0;
				data.transformNanoseconds = 0;
			}

			struct ThreadProfile
			{
				ProfileData data;
				std::atomic<bool> active{ false };
				uint16_t previousOpcode = kNoOpcode;

				ThreadProfile()
				{
					ProfileRegistry& registry = GetProfileRegistry();
					std::lock_guard<std::mutex> lock(registry.profilesMutex);
					registry.liveProfiles.push_back(this);
				}

				~ThreadProfile()
				{
					ProfileRegistry& registry = GetProfileRegistry();
					std::lock_guard<std::mutex> lock(registry.profilesMutex);
					MergeProfileData(registry.retiredData, data);
					auto it = std::find(registry.liveProfiles.begin(), registry.liveProfiles.end(), this);
					if (it != registry.liveProfiles.end())
					{
						registry.liveProfiles.erase(it);
					}
				}
			};

			ThreadProfile& GetThreadProfile()
			{
				thread_local ThreadProfile profile;
				return profile;
			}

			ThreadProfile* BeginRecord()
			{
				ProfileRegistry& registry = GetProfileRegistry();
				if (!registry.recording.load(std::memory_order_seq_cst))
				{
					return nullptr;
				}
				ThreadProfile& profile = GetThreadProfile();
				profile.active.store(true, std::memory_order_seq_cst);
				if (!registry.recording.load(std::memory_order_seq_cst))
				{
					profile.active.store(false, std::memory_order_seq_cst);
					return nullptr;
				}
				return &profile;
			}

			void EndRecord(ThreadProfile& profile)
			{
				profile.active.store(false, std::memory_order_seq_cst);
			}

			void WaitForActiveRecords(ProfileRegistry& registry)
			{
				for (ThreadProfile* profile : registry.liveProfiles)
				{
					while (profile->active.load(std::memory_order_seq_cst))
					{
						std::this_thread::yield();
					}
				}
			}
		}

		void InterpreterProfile::Reset()
		{
			ProfileRegistry& registry = GetProfileRegistry();
			std::lock_guard<std::mutex> pauseLock(registry.pauseMutex);
			registry.recording.store(false, std::memory_order_seq_cst);
			{
				std::lock_guard<std::mutex> profilesLock(registry.profilesMutex);
				WaitForActiveRecords(registry);
				ClearProfileData(registry.retiredData);
				for (ThreadProfile* profile : registry.liveProfiles)
				{
					ClearProfileData(profile->data);
					profile->previousOpcode = kNoOpcode;
				}
			}
			registry.recording.store(true, std::memory_order_seq_cst);
		}

		void InterpreterProfile::RecordInterpreterEntry()
		{
			ThreadProfile* profile = BeginRecord();
			if (profile == nullptr)
			{
				return;
			}
			++profile->data.interpreterEntryCount;
			profile->previousOpcode = kNoOpcode;
			EndRecord(*profile);
		}

		void InterpreterProfile::RecordDispatch(uint16_t opcode)
		{
			if (opcode >= kOpcodeCount)
			{
				return;
			}
			ThreadProfile* profile = BeginRecord();
			if (profile == nullptr)
			{
				return;
			}
			++profile->data.dispatchCount;
			++profile->data.opcodeCounts[opcode];
			if (profile->previousOpcode != kNoOpcode)
			{
				uint32_t key = (static_cast<uint32_t>(profile->previousOpcode) << 16) | opcode;
				++profile->data.transitionCounts[key];
			}
			profile->previousOpcode = opcode;
			EndRecord(*profile);
		}

		void InterpreterProfile::RecordTransform(uint64_t nanoseconds)
		{
			ThreadProfile* profile = BeginRecord();
			if (profile == nullptr)
			{
				return;
			}
			++profile->data.transformCount;
			profile->data.transformNanoseconds += nanoseconds;
			EndRecord(*profile);
		}

		Il2CppString* InterpreterProfile::Snapshot()
		{
			ProfileData aggregate;
			ProfileRegistry& registry = GetProfileRegistry();
			{
				std::lock_guard<std::mutex> pauseLock(registry.pauseMutex);
				registry.recording.store(false, std::memory_order_seq_cst);
				{
					std::lock_guard<std::mutex> profilesLock(registry.profilesMutex);
					WaitForActiveRecords(registry);
					MergeProfileData(aggregate, registry.retiredData);
					for (ThreadProfile* profile : registry.liveProfiles)
					{
						MergeProfileData(aggregate, profile->data);
					}
				}
				registry.recording.store(true, std::memory_order_seq_cst);
			}

			std::vector<std::pair<uint16_t, uint64_t>> opcodes;
			for (uint16_t i = 0; i < kOpcodeCount; i++)
			{
				if (aggregate.opcodeCounts[i] != 0)
				{
					opcodes.emplace_back(i, aggregate.opcodeCounts[i]);
				}
			}
			std::sort(opcodes.begin(), opcodes.end(), [](const auto& left, const auto& right) {
				return left.second > right.second;
			});

			std::vector<std::pair<uint32_t, uint64_t>> transitions(
				aggregate.transitionCounts.begin(), aggregate.transitionCounts.end());
			std::sort(transitions.begin(), transitions.end(), [](const auto& left, const auto& right) {
				return left.second > right.second;
			});

			std::ostringstream json;
			json << "{\"schemaVersion\":1,\"dispatchCount\":" << aggregate.dispatchCount
				<< ",\"interpreterEntryCount\":" << aggregate.interpreterEntryCount
				<< ",\"transformCount\":" << aggregate.transformCount
				<< ",\"transformNanoseconds\":" << aggregate.transformNanoseconds << ",\"opcodes\":[";
			for (size_t i = 0; i < opcodes.size(); i++)
			{
				if (i != 0) json << ',';
				json << "{\"id\":" << opcodes[i].first << ",\"count\":" << opcodes[i].second << '}';
			}
			json << "],\"transitions\":[";
			for (size_t i = 0; i < transitions.size(); i++)
			{
				if (i != 0) json << ',';
				uint16_t from = static_cast<uint16_t>(transitions[i].first >> 16);
				uint16_t to = static_cast<uint16_t>(transitions[i].first & 0xffff);
				json << "{\"from\":" << from << ",\"to\":" << to << ",\"count\":" << transitions[i].second << '}';
			}
			json << "]}";
			return il2cpp::vm::String::New(json.str().c_str());
		}
	}

#endif

	void RuntimeApi::RegisterInternalCalls()
	{
		il2cpp::vm::InternalCalls::Add("HybridCLR.Lab.CurrentStorageRuntime::Load(System.Byte[][],System.Byte[][],System.Byte[][],System.UInt32[][],System.UInt32[][])", (Il2CppMethodPointer)LoadDhePayloadsWithExecutionPlan);
		il2cpp::vm::InternalCalls::Add("HybridCLR.Lab.CurrentStorageRuntime::Resolve(System.Reflection.MethodInfo)", (Il2CppMethodPointer)ResolveDheCurrentStorageProbe);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadMetadataForAOTAssembly(System.Byte[],HybridCLR.HomologousImageMode)", (Il2CppMethodPointer)LoadMetadataForAOTAssembly);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadDifferentialHybridAssemblyWithMetaVersion(System.Byte[],System.Byte[],System.Byte[])", (Il2CppMethodPointer)LoadDifferentialHybridAssemblyWithMetaVersion);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadDifferentialHybridAssembliesWithMetaVersion(System.Byte[][],System.Byte[][],System.Byte[][])", (Il2CppMethodPointer)LoadDifferentialHybridAssembliesWithMetaVersion);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadDifferentialHybridAssembliesWithMetaVersionAndExecutionPlan(System.Byte[][],System.Byte[][],System.Byte[][],System.UInt32[][],System.UInt32[][])", (Il2CppMethodPointer)LoadDifferentialHybridAssembliesWithMetaVersionAndExecutionPlan);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadDifferentialHybridAssembliesWithMetaVersionAndExecutionPlanAndSources(System.Byte[][],System.Byte[][],System.Byte[][],System.UInt32[][],System.UInt32[][],System.Int32[],System.UInt32[][])", (Il2CppMethodPointer)LoadDifferentialHybridAssembliesWithMetaVersionAndExecutionPlanAndSources);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadDifferentialHybridAssemblySources(System.Byte[][],System.Byte[][],System.Byte[][],System.UInt32[][],System.UInt32[][],System.Int32[],System.UInt32[][],System.UInt32[][])", (Il2CppMethodPointer)LoadDifferentialHybridAssemblySources);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadDifferentialHybridAssemblyBatch(System.Byte[][],System.Byte[][],System.Byte[][],System.UInt32[][],System.UInt32[][],System.Int32[],System.UInt32[][],System.UInt32[][],System.Byte[][])", (Il2CppMethodPointer)LoadDifferentialHybridAssemblyBatch);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::IsDifferentialMethodChanged(System.Reflection.MethodInfo)", (Il2CppMethodPointer)IsDifferentialMethodChanged);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::GetDifferentialInterpreterEntryCount()", (Il2CppMethodPointer)GetDifferentialInterpreterEntryCount);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::GetDifferentialAotBridgeCallCount()", (Il2CppMethodPointer)GetDifferentialAotBridgeCallCount);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::GetDifferentialAotEntryCount()", (Il2CppMethodPointer)GetDifferentialAotEntryCount);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::ResetDifferentialDispatchCounters()", (Il2CppMethodPointer)ResetDifferentialDispatchCounters);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::GetRuntimeOption(HybridCLR.RuntimeOptionId)", (Il2CppMethodPointer)GetRuntimeOption);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::SetRuntimeOption(HybridCLR.RuntimeOptionId,System.Int32)", (Il2CppMethodPointer)SetRuntimeOption);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmMethod(System.Reflection.MethodInfo)", (Il2CppMethodPointer)PrewarmMethod);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmMethodBase(System.Reflection.MethodBase)", (Il2CppMethodPointer)PrewarmMethodBase);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmMethodBaseBatch(System.Reflection.MethodBase[],System.Int32)", (Il2CppMethodPointer)PrewarmMethodBaseBatch);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmMethodBaseBatchResultMask(System.Reflection.MethodBase[],System.Int32)", (Il2CppMethodPointer)PrewarmMethodBaseBatchResultMask);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmMethodToken(System.Type,System.Int32)", (Il2CppMethodPointer)PrewarmMethodToken);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmMethodTokenBatch(System.Type[],System.Int32[],System.Int32)", (Il2CppMethodPointer)PrewarmMethodTokenBatch);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmMethodTokenBatchResultMask(System.Type[],System.Int32[],System.Int32)", (Il2CppMethodPointer)PrewarmMethodTokenBatchResultMask);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PreJitClass(System.Type)", (Il2CppMethodPointer)PreJitClass);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PreJitMethod(System.Reflection.MethodInfo)", (Il2CppMethodPointer)PreJitMethod);
	#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		il2cpp::vm::InternalCalls::Add("HybridCLR.Lab.Instrumentation::Reset()", (Il2CppMethodPointer)interpreter::InterpreterProfile::Reset);
		il2cpp::vm::InternalCalls::Add("HybridCLR.Lab.Instrumentation::Snapshot()", (Il2CppMethodPointer)interpreter::InterpreterProfile::Snapshot);
	#endif
	#if defined(HYBRIDCLR_LAB_FGS_TESTS)
		il2cpp::vm::InternalCalls::Add("HybridCLR.Lab.Instrumentation::ResetFullGenericSharing()", (Il2CppMethodPointer)interpreter::FullGenericSharingDiagnostics::Reset);
		il2cpp::vm::InternalCalls::Add("HybridCLR.Lab.Instrumentation::GetFullGenericSharingDispatchCount()", (Il2CppMethodPointer)interpreter::FullGenericSharingDiagnostics::GetDispatchCount);
		il2cpp::vm::InternalCalls::Add("HybridCLR.Lab.Instrumentation::GetFullGenericSharingInterpreterInvokerCount()", (Il2CppMethodPointer)interpreter::FullGenericSharingDiagnostics::GetInterpreterInvokerCount);
	#endif
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmClass(System.Type)", (Il2CppMethodPointer)PrewarmClass);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmClassBatch(System.Type[],System.Int32)", (Il2CppMethodPointer)PrewarmClassBatch);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::PrewarmClassBatchResultMask(System.Type[],System.Int32)", (Il2CppMethodPointer)PrewarmClassBatchResultMask);
#if defined(HYBRIDCLR_LAB_INSTRUMENTED)
		il2cpp::vm::InternalCalls::Add("HybridCLR.Lab.Instrumentation::FlushMetadataProfile()", (Il2CppMethodPointer)metadata::FlushMetadataProfile);
#endif
	}

	int32_t RuntimeApi::LoadMetadataForAOTAssembly(Il2CppArray* dllBytes, int32_t mode)
	{
		if (!dllBytes)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		return (int32_t)hybridclr::metadata::Assembly::LoadMetadataForAOTAssembly(il2cpp::vm::Array::GetFirstElementAddress(dllBytes), il2cpp::vm::Array::GetByteLength(dllBytes), (hybridclr::metadata::HomologousImageMode)mode);
	}

	int32_t RuntimeApi::LoadDifferentialHybridAssemblyWithMetaVersion(Il2CppArray* dllBytes,
		Il2CppArray* baseMvBytes, Il2CppArray* currentMvBytes)
	{
		if (!dllBytes || !baseMvBytes || !currentMvBytes)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		DheLoadPayload payload;
		int32_t parseError = ParseDheLoadPayload(dllBytes, baseMvBytes,
			currentMvBytes, payload);
		if (parseError != (int32_t)metadata::LoadImageErrorCode::OK)
		{
			return parseError;
		}
		std::vector<DheLoadPayload> payloads;
		payloads.push_back(std::move(payload));
		return LoadDhePayloads(payloads);
	}

	int32_t RuntimeApi::LoadDifferentialHybridAssembliesWithMetaVersion(
		Il2CppArray* dllBytes, Il2CppArray* baseMvBytes, Il2CppArray* currentMvBytes)
	{
		if (!dllBytes || !baseMvBytes || !currentMvBytes)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		const uint32_t count = il2cpp::vm::Array::GetLength(dllBytes);
		if (count == 0 || il2cpp::vm::Array::GetLength(baseMvBytes) != count ||
			il2cpp::vm::Array::GetLength(currentMvBytes) != count)
		{
			return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
		}

		Il2CppArray** dllArray = reinterpret_cast<Il2CppArray**>(
			il2cpp::vm::Array::GetFirstElementAddress(dllBytes));
		Il2CppArray** baseMvArray = reinterpret_cast<Il2CppArray**>(
			il2cpp::vm::Array::GetFirstElementAddress(baseMvBytes));
		Il2CppArray** currentMvArray = reinterpret_cast<Il2CppArray**>(
			il2cpp::vm::Array::GetFirstElementAddress(currentMvBytes));
		std::vector<DheLoadPayload> payloads(count);
		std::unordered_set<std::string> assemblyNames;
		for (uint32_t index = 0; index < count; ++index)
		{
			int32_t parseError = ParseDheLoadPayload(dllArray[index],
				baseMvArray[index], currentMvArray[index], payloads[index]);
			if (parseError != (int32_t)metadata::LoadImageErrorCode::OK)
			{
				return parseError;
			}
			if (!assemblyNames.insert(
					payloads[index].baseMetaVersion.assemblyName).second)
			{
				return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
			}
		}
		return LoadDhePayloads(payloads);
	}

	int32_t RuntimeApi::LoadDifferentialHybridAssembliesWithMetaVersionAndExecutionPlan(
		Il2CppArray* dllBytes, Il2CppArray* baseMvBytes, Il2CppArray* currentMvBytes,
		Il2CppArray* typeSelections, Il2CppArray* methodSelections)
	{
		return LoadDhePayloadsWithExecutionPlan(dllBytes, baseMvBytes, currentMvBytes,
			typeSelections, methodSelections);
	}

	int32_t RuntimeApi::LoadDifferentialHybridAssembliesWithMetaVersionAndExecutionPlanAndSources(
		Il2CppArray* dllBytes, Il2CppArray* baseMvBytes, Il2CppArray* currentMvBytes,
		Il2CppArray* typeSelections, Il2CppArray* methodSelections,
		Il2CppArray* sourceKinds, Il2CppArray* excludedTypeSelections)
	{
		return LoadDhePayloadsWithExecutionPlanAndSources(dllBytes, baseMvBytes, currentMvBytes,
			typeSelections, methodSelections, sourceKinds, excludedTypeSelections);
	}

	int32_t RuntimeApi::LoadDifferentialHybridAssemblySources(
		Il2CppArray* dllBytes, Il2CppArray* baseMvBytes, Il2CppArray* currentMvBytes,
		Il2CppArray* typeSelections, Il2CppArray* methodSelections, Il2CppArray* sourceKinds,
		Il2CppArray* excludedTypeSelections, Il2CppArray* genericContextSelections)
	{
		if (!sourceKinds || !excludedTypeSelections || !genericContextSelections)
			return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
		return LoadDhePayloadsWithExecutionPlanAndSources(dllBytes, baseMvBytes, currentMvBytes,
			typeSelections, methodSelections, sourceKinds, excludedTypeSelections, genericContextSelections);
	}

	int32_t RuntimeApi::LoadDifferentialHybridAssemblyBatch(
		Il2CppArray* dllBytes, Il2CppArray* baseMvBytes, Il2CppArray* currentMvBytes,
		Il2CppArray* typeSelections, Il2CppArray* methodSelections, Il2CppArray* sourceKinds,
		Il2CppArray* excludedTypeSelections, Il2CppArray* genericContextSelections, Il2CppArray* interpreterDlls)
	{
		if (!sourceKinds || !excludedTypeSelections || !genericContextSelections || !interpreterDlls)
			return (int32_t)metadata::LoadImageErrorCode::DHE_MV_BAD_FORMAT;
		return LoadDhePayloadsWithExecutionPlanAndSources(dllBytes, baseMvBytes, currentMvBytes,
			typeSelections, methodSelections, sourceKinds, excludedTypeSelections, genericContextSelections, interpreterDlls);
	}

	int32_t RuntimeApi::IsDifferentialMethodChanged(Il2CppReflectionMethod* method)
	{
		return method && method->method && hybridclr::dhe::IsChangedMethod(method->method);
	}

	int32_t RuntimeApi::GetDifferentialInterpreterEntryCount()
	{
		return hybridclr::dhe::GetInterpreterEntryCount();
	}

	int32_t RuntimeApi::GetDifferentialAotBridgeCallCount()
	{
		return hybridclr::dhe::GetAotBridgeCallCount();
	}

	int32_t RuntimeApi::GetDifferentialAotEntryCount()
	{
		return hybridclr::dhe::GetAotEntryCount();
	}

	void RuntimeApi::ResetDifferentialDispatchCounters()
	{
		hybridclr::dhe::ResetDispatchCounters();
	}

	int32_t RuntimeApi::GetRuntimeOption(int32_t optionId)
	{
		return hybridclr::RuntimeConfig::GetRuntimeOption((hybridclr::RuntimeOptionId)optionId);
	}

	void RuntimeApi::SetRuntimeOption(int32_t optionId, int32_t value)
	{
		hybridclr::RuntimeConfig::SetRuntimeOption((hybridclr::RuntimeOptionId)optionId, value);
	}

	int32_t PreJitMethod0(const MethodInfo* methodInfo);
	bool PreJitMethodEligible(const MethodInfo* methodInfo);
	bool PrewarmMethodEligible(const MethodInfo* methodInfo);
	int32_t PrewarmMethod0(const MethodInfo* methodInfo, bool metadataLockHeld = false);

	static Il2CppClass* ResolvePrewarmClass(Il2CppReflectionType* type)
	{
		if (!type || !type->type)
			return nullptr;
		if (metadata::HasNotInstantiatedGenericType(type->type))
			return nullptr;
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type->type, false);
		if (!klass)
			return nullptr;
		metadata::Image* image = metadata::MetadataModule::GetImage(klass->image);
		if (!image)
		{
			image = (metadata::Image*)hybridclr::metadata::AOTHomologousImage::FindImageByAssembly(
				klass->rank ? il2cpp_defaults.corlib->assembly : klass->image->assembly);
			if (!image)
				return nullptr;
		}
		return klass;
	}

	static int32_t PrewarmClassInternal(Il2CppReflectionType* type)
	{
		Il2CppClass* klass = ResolvePrewarmClass(type);
		if (!klass)
			return false;

		il2cpp::vm::Class::Init(klass);
		if (klass->initializationExceptionGCHandle)
			return false;
		// Tuanjie's lazy-init mode intentionally leaves these tables unresolved
		// after Class::Init. Prewarm must force the same metadata that reflection
		// and the first interpreter call would otherwise initialize on demand.
		il2cpp::vm::Class::SetupFields(klass);
		il2cpp::vm::Class::SetupMethods(klass);
		il2cpp::vm::Class::SetupVTable(klass);
		il2cpp::vm::Class::SetupInterfaces(klass);
		il2cpp::vm::Class::SetupNestedTypes(klass);
		il2cpp::vm::Class::SetupProperties(klass);
		il2cpp::vm::Class::SetupEvents(klass);
		if (klass->method_count != 0 && !klass->methods)
			return false;

		bool allMethodsReady = true;
		// Keep interpreter transformations in one lock scope for this class.
		il2cpp::os::FastAutoLock metadataLock(&il2cpp::vm::g_MetadataLock);
		for (uint16_t i = 0; i < klass->method_count; i++)
		{
			const MethodInfo* methodInfo = klass->methods[i];
			if (!methodInfo)
				allMethodsReady = false;
			else if (PrewarmMethodEligible(methodInfo) && !PrewarmMethod0(methodInfo, true))
				allMethodsReady = false;
		}
		return allMethodsReady;
	}

	int32_t RuntimeApi::PrewarmClass(Il2CppReflectionType* type)
	{
		return PrewarmClassInternal(type);
	}

	int32_t RuntimeApi::PrewarmClassBatch(Il2CppArray* types, int32_t count)
	{
		if (!types)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		uint32_t length = il2cpp::vm::Array::GetLength(types);
		if (count < 0 || static_cast<uint32_t>(count) > length)
		{
			il2cpp::vm::Exception::RaiseArgumentOutOfRangeException("count");
		}

		Il2CppReflectionType** typeArray = reinterpret_cast<Il2CppReflectionType**>(
			il2cpp::vm::Array::GetFirstElementAddress(types));
		bool allReady = true;
		for (int32_t index = 0; index < count; index++)
		{
			if (!PrewarmClassInternal(typeArray[index]))
				allReady = false;
		}
		return allReady;
	}

	int32_t RuntimeApi::PrewarmClassBatchResultMask(Il2CppArray* types, int32_t count)
	{
		if (!types)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		uint32_t length = il2cpp::vm::Array::GetLength(types);
		if (count < 0 || count > 32 || static_cast<uint32_t>(count) > length)
		{
			il2cpp::vm::Exception::RaiseArgumentOutOfRangeException("count");
		}

		Il2CppReflectionType** typeArray = reinterpret_cast<Il2CppReflectionType**>(
			il2cpp::vm::Array::GetFirstElementAddress(types));
		uint32_t failureMask = 0;
		for (int32_t index = 0; index < count; index++)
		{
			if (!PrewarmClassInternal(typeArray[index]))
				failureMask |= (uint32_t)1 << index;
		}
		return static_cast<int32_t>(failureMask);
	}

	int32_t RuntimeApi::PreJitClass(Il2CppReflectionType* type)
	{
		if (!type || !type->type || metadata::HasNotInstantiatedGenericType(type->type))
			return false;
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type->type, false);
		if (!klass)
			return false;
		metadata::Image* image = metadata::MetadataModule::GetImage(klass->image);
		if (!image)
		{
			image = (metadata::Image*)hybridclr::metadata::AOTHomologousImage::FindImageByAssembly(
				klass->rank ? il2cpp_defaults.corlib->assembly : klass->image->assembly);
			if (!image)
				return false;
		}
		// In lazy-init players Class::Init does not materialize the method table.
		// Keep the legacy PreJitClass API safe when it is called before reflection
		// has touched the type.
		il2cpp::vm::Class::SetupMethods(klass);
		if (klass->method_count != 0 && !klass->methods)
			return false;
		for (uint16_t i = 0; i < klass->method_count; i++)
		{
			const MethodInfo* methodInfo = klass->methods[i];
			PreJitMethod0(methodInfo);
		}
		return true;
	}

	bool PrewarmMethodEligible(const MethodInfo* methodInfo)
	{
		if (!methodInfo || !methodInfo->klass)
			return false;
		if (metadata::HasNotInstantiatedGenericType(&methodInfo->klass->byval_arg))
			return false;
		if (!methodInfo->is_inflated)
			return !methodInfo->is_generic;
		const Il2CppGenericMethod* genericMethod = methodInfo->genericMethod;
		return genericMethod &&
			!metadata::HasNotInstantiatedGenericType(genericMethod->context.class_inst) &&
			!metadata::HasNotInstantiatedGenericType(genericMethod->context.method_inst);
	}

	int32_t PrewarmMethod0(const MethodInfo* methodInfo, bool metadataLockHeld)
	{
		if (!PrewarmMethodEligible(methodInfo))
			return false;
		// AOT methods already have a native entry point. Class::Init still makes
		// the declaring type's metadata state safe for a subsequent first call.
		if (!metadataLockHeld)
		{
			il2cpp::vm::Class::Init(methodInfo->klass);
			if (methodInfo->klass->initializationExceptionGCHandle)
				return false;
		}
		if (!methodInfo->isInterpterImpl)
			return true;
		return interpreter::InterpreterModule::GetInterpMethodInfo(methodInfo, metadataLockHeld) != nullptr;
	}

	int32_t RuntimeApi::PrewarmMethod(Il2CppReflectionMethod* method)
	{
		if (!method || !method->method)
			return false;
		return PrewarmMethod0(method->method);
	}

	int32_t RuntimeApi::PrewarmMethodBase(Il2CppReflectionMethod* method)
	{
		if (!method || !method->method)
			return false;
		return PrewarmMethod0(method->method);
	}

	int32_t RuntimeApi::PrewarmMethodBaseBatch(Il2CppArray* methods, int32_t count)
	{
		if (!methods)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		uint32_t length = il2cpp::vm::Array::GetLength(methods);
		if (count < 0 || static_cast<uint32_t>(count) > length)
		{
			il2cpp::vm::Exception::RaiseArgumentOutOfRangeException("count");
		}

		Il2CppReflectionMethod** methodArray = reinterpret_cast<Il2CppReflectionMethod**>(
			il2cpp::vm::Array::GetFirstElementAddress(methods));
		std::vector<const MethodInfo*> eligibleMethods;
		eligibleMethods.reserve(static_cast<size_t>(count));
		std::vector<Il2CppClass*> initializedClasses;
		initializedClasses.reserve(static_cast<size_t>(count));
		bool allReady = true;
		for (int32_t index = 0; index < count; index++)
		{
			Il2CppReflectionMethod* reflectionMethod = methodArray[index];
			if (!reflectionMethod || !reflectionMethod->method ||
				!PrewarmMethodEligible(reflectionMethod->method))
			{
				allReady = false;
				continue;
			}

			const MethodInfo* methodInfo = reflectionMethod->method;
			bool classInitialized = false;
			for (Il2CppClass* initializedClass : initializedClasses)
			{
				if (initializedClass == methodInfo->klass)
				{
					classInitialized = true;
					break;
				}
			}
			if (!classInitialized)
			{
				il2cpp::vm::Class::Init(methodInfo->klass);
				initializedClasses.push_back(methodInfo->klass);
			}
			if (methodInfo->klass->initializationExceptionGCHandle)
			{
				allReady = false;
				continue;
			}
			eligibleMethods.push_back(methodInfo);
		}

		if (!eligibleMethods.empty())
		{
			il2cpp::os::FastAutoLock metadataLock(&il2cpp::vm::g_MetadataLock);
			for (const MethodInfo* methodInfo : eligibleMethods)
			{
				if (!PrewarmMethod0(methodInfo, true))
					allReady = false;
			}
		}
		return allReady;
	}

	int32_t RuntimeApi::PrewarmMethodBaseBatchResultMask(Il2CppArray* methods, int32_t count)
	{
		if (!methods)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		uint32_t length = il2cpp::vm::Array::GetLength(methods);
		if (count < 0 || count > 32 || static_cast<uint32_t>(count) > length)
		{
			il2cpp::vm::Exception::RaiseArgumentOutOfRangeException("count");
		}

		Il2CppReflectionMethod** methodArray = reinterpret_cast<Il2CppReflectionMethod**>(
			il2cpp::vm::Array::GetFirstElementAddress(methods));
		std::vector<const MethodInfo*> resolvedMethods(static_cast<size_t>(count), nullptr);
		std::vector<Il2CppClass*> initializedClasses;
		initializedClasses.reserve(static_cast<size_t>(count));
		uint32_t failureMask = 0;
		for (int32_t index = 0; index < count; index++)
		{
			Il2CppReflectionMethod* reflectionMethod = methodArray[index];
			if (!reflectionMethod || !reflectionMethod->method ||
				!PrewarmMethodEligible(reflectionMethod->method))
			{
				failureMask |= (uint32_t)1 << index;
				continue;
			}

			const MethodInfo* methodInfo = reflectionMethod->method;
			bool classInitialized = false;
			for (Il2CppClass* initializedClass : initializedClasses)
			{
				if (initializedClass == methodInfo->klass)
				{
					classInitialized = true;
					break;
				}
			}
			if (!classInitialized)
			{
				il2cpp::vm::Class::Init(methodInfo->klass);
				initializedClasses.push_back(methodInfo->klass);
			}
			if (methodInfo->klass->initializationExceptionGCHandle)
			{
				failureMask |= (uint32_t)1 << index;
				continue;
			}
			resolvedMethods[index] = methodInfo;
		}

		if (count != 0)
		{
			il2cpp::os::FastAutoLock metadataLock(&il2cpp::vm::g_MetadataLock);
			for (int32_t index = 0; index < count; index++)
			{
				const MethodInfo* methodInfo = resolvedMethods[index];
				if (methodInfo && !PrewarmMethod0(methodInfo, true))
					failureMask |= (uint32_t)1 << index;
			}
		}
		return static_cast<int32_t>(failureMask);
	}

	static const MethodInfo* ResolvePrewarmMethodToken(Il2CppReflectionType* type, int32_t metadataToken)
	{
		if (metadataToken <= 0 || metadata::DecodeTokenTableType(static_cast<uint32_t>(metadataToken)) != metadata::TableType::METHOD)
			return nullptr;
		Il2CppClass* klass = ResolvePrewarmClass(type);
		if (!klass || klass->generic_class || !metadata::IsInterpreterType(klass))
			return nullptr;

		const Il2CppTypeDefinition* typeDefinition = reinterpret_cast<const Il2CppTypeDefinition*>(klass->typeMetadataHandle);
		if (!typeDefinition || typeDefinition->genericContainerIndex != kGenericContainerIndexInvalid)
			return nullptr;
		metadata::InterpreterImage* image = metadata::MetadataModule::GetImage(klass);
		if (!image)
			return nullptr;

		const uint32_t methodRow = metadata::DecodeTokenRowIndex(static_cast<uint32_t>(metadataToken));
		if (methodRow == 0)
			return nullptr;
		const uint32_t methodIndex = methodRow - 1;
		const uint32_t methodStart = metadata::DecodeMetadataIndex(typeDefinition->methodStart);
		if (methodIndex < methodStart || methodIndex - methodStart >= typeDefinition->method_count)
			return nullptr;

		const Il2CppMethodDefinition* methodDefinition = image->GetMethodDefinitionFromRawIndex(methodIndex);
		if (methodDefinition->token != static_cast<uint32_t>(metadataToken) ||
			methodDefinition->genericContainerIndex != kGenericContainerIndexInvalid)
		{
			return nullptr;
		}
		return metadata::MetadataModule::GetMethodInfoFromMethodDefinition(methodDefinition);
	}

	int32_t RuntimeApi::PrewarmMethodToken(Il2CppReflectionType* declaringType, int32_t metadataToken)
	{
		const MethodInfo* methodInfo = ResolvePrewarmMethodToken(declaringType, metadataToken);
		return methodInfo ? PrewarmMethod0(methodInfo) : false;
	}

	int32_t RuntimeApi::PrewarmMethodTokenBatch(Il2CppArray* declaringTypes, Il2CppArray* metadataTokens, int32_t count)
	{
		if (!declaringTypes || !metadataTokens)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		uint32_t typeLength = il2cpp::vm::Array::GetLength(declaringTypes);
		uint32_t tokenLength = il2cpp::vm::Array::GetLength(metadataTokens);
		if (count < 0 || static_cast<uint32_t>(count) > typeLength || static_cast<uint32_t>(count) > tokenLength)
		{
			il2cpp::vm::Exception::RaiseArgumentOutOfRangeException("count");
		}

		Il2CppReflectionType** typeArray = reinterpret_cast<Il2CppReflectionType**>(
			il2cpp::vm::Array::GetFirstElementAddress(declaringTypes));
		int32_t* tokenArray = reinterpret_cast<int32_t*>(
			il2cpp::vm::Array::GetFirstElementAddress(metadataTokens));
		std::vector<const MethodInfo*> methods;
		methods.reserve(static_cast<size_t>(count));
		std::vector<Il2CppClass*> initializedClasses;
		initializedClasses.reserve(static_cast<size_t>(count));
		bool allReady = true;
		for (int32_t index = 0; index < count; index++)
		{
			const MethodInfo* methodInfo = ResolvePrewarmMethodToken(typeArray[index], tokenArray[index]);
			if (!methodInfo)
			{
				allReady = false;
				continue;
			}
			bool classInitialized = false;
			for (Il2CppClass* initializedClass : initializedClasses)
			{
				if (initializedClass == methodInfo->klass)
				{
					classInitialized = true;
					break;
				}
			}
			if (!classInitialized)
			{
				il2cpp::vm::Class::Init(methodInfo->klass);
				initializedClasses.push_back(methodInfo->klass);
			}
			if (methodInfo->klass->initializationExceptionGCHandle)
			{
				allReady = false;
				continue;
			}
			methods.push_back(methodInfo);
		}

		if (!methods.empty())
		{
			il2cpp::os::FastAutoLock metadataLock(&il2cpp::vm::g_MetadataLock);
			for (const MethodInfo* methodInfo : methods)
			{
				if (!PrewarmMethod0(methodInfo, true))
					allReady = false;
			}
		}
		return allReady;
	}

	int32_t RuntimeApi::PrewarmMethodTokenBatchResultMask(Il2CppArray* declaringTypes, Il2CppArray* metadataTokens, int32_t count)
	{
		if (!declaringTypes || !metadataTokens)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		uint32_t typeLength = il2cpp::vm::Array::GetLength(declaringTypes);
		uint32_t tokenLength = il2cpp::vm::Array::GetLength(metadataTokens);
		if (count < 0 || count > 32 || static_cast<uint32_t>(count) > typeLength || static_cast<uint32_t>(count) > tokenLength)
		{
			il2cpp::vm::Exception::RaiseArgumentOutOfRangeException("count");
		}

		Il2CppReflectionType** typeArray = reinterpret_cast<Il2CppReflectionType**>(
			il2cpp::vm::Array::GetFirstElementAddress(declaringTypes));
		int32_t* tokenArray = reinterpret_cast<int32_t*>(
			il2cpp::vm::Array::GetFirstElementAddress(metadataTokens));
		std::vector<const MethodInfo*> methods(static_cast<size_t>(count), nullptr);
		std::vector<Il2CppClass*> initializedClasses;
		initializedClasses.reserve(static_cast<size_t>(count));
		uint32_t failureMask = 0;
		for (int32_t index = 0; index < count; index++)
		{
			methods[index] = ResolvePrewarmMethodToken(typeArray[index], tokenArray[index]);
			if (!methods[index])
			{
				failureMask |= (uint32_t)1 << index;
				continue;
			}
			Il2CppClass* klass = methods[index]->klass;
			bool classInitialized = false;
			for (Il2CppClass* initializedClass : initializedClasses)
			{
				if (initializedClass == klass)
				{
					classInitialized = true;
					break;
				}
			}
			if (!classInitialized)
			{
				il2cpp::vm::Class::Init(klass);
				initializedClasses.push_back(klass);
			}
			if (klass->initializationExceptionGCHandle)
			{
				failureMask |= (uint32_t)1 << index;
				methods[index] = nullptr;
			}
		}

		if (count != 0)
		{
			il2cpp::os::FastAutoLock metadataLock(&il2cpp::vm::g_MetadataLock);
			for (int32_t index = 0; index < count; index++)
			{
				const MethodInfo* methodInfo = methods[index];
				if (methodInfo && !PrewarmMethod0(methodInfo, true))
					failureMask |= (uint32_t)1 << index;
			}
		}
		return static_cast<int32_t>(failureMask);
	}

	bool PreJitMethodEligible(const MethodInfo* methodInfo)
	{
		if (!methodInfo || !methodInfo->isInterpterImpl || !methodInfo->klass || methodInfo->klass->is_generic)
			return false;
		if (!methodInfo->is_inflated)
			return !methodInfo->is_generic;
		const Il2CppGenericMethod* genericMethod = methodInfo->genericMethod;
		return genericMethod &&
			!metadata::HasNotInstantiatedGenericType(genericMethod->context.class_inst) &&
			!metadata::HasNotInstantiatedGenericType(genericMethod->context.method_inst);
	}

	int32_t PreJitMethod0(const MethodInfo* methodInfo)
	{
		if (!PreJitMethodEligible(methodInfo))
			return false;
		return interpreter::InterpreterModule::GetInterpMethodInfo(methodInfo) != nullptr;
	}

	int32_t RuntimeApi::PreJitMethod(Il2CppReflectionMethod* method)
	{
		if (!method || !method->method)
			return false;
		return PreJitMethod0(method->method);
	}
}
