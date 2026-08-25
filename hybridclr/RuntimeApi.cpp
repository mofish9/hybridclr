#include "RuntimeApi.h"

#include "codegen/il2cpp-codegen.h"
#include "vm/InternalCalls.h"
#include "vm/Array.h"
#include "vm/Exception.h"
#include "vm/Class.h"

#include "metadata/MetadataModule.h"
#include "metadata/MetadataUtil.h"
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

namespace hybridclr
{
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
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::LoadMetadataForAOTAssembly(System.Byte[],HybridCLR.HomologousImageMode)", (Il2CppMethodPointer)LoadMetadataForAOTAssembly);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::GetRuntimeOption(HybridCLR.RuntimeOptionId)", (Il2CppMethodPointer)GetRuntimeOption);
		il2cpp::vm::InternalCalls::Add("HybridCLR.RuntimeApi::SetRuntimeOption(HybridCLR.RuntimeOptionId,System.Int32)", (Il2CppMethodPointer)SetRuntimeOption);
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
	}

	int32_t RuntimeApi::LoadMetadataForAOTAssembly(Il2CppArray* dllBytes, int32_t mode)
	{
		if (!dllBytes)
		{
			il2cpp::vm::Exception::RaiseNullReferenceException();
		}
		return (int32_t)hybridclr::metadata::Assembly::LoadMetadataForAOTAssembly(il2cpp::vm::Array::GetFirstElementAddress(dllBytes), il2cpp::vm::Array::GetByteLength(dllBytes), (hybridclr::metadata::HomologousImageMode)mode);
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

	int32_t RuntimeApi::PreJitClass(Il2CppReflectionType* type)
	{
		if (metadata::HasNotInstantiatedGenericType(type->type))
		{
			return false;
		}
		Il2CppClass* klass = il2cpp::vm::Class::FromIl2CppType(type->type, false);
		if (!klass)
		{
			return false;
		}
		metadata::Image* image = metadata::MetadataModule::GetImage(klass->image);
		if (!image)
		{
			image = (metadata::Image*)hybridclr::metadata::AOTHomologousImage::FindImageByAssembly(
				klass->rank ? il2cpp_defaults.corlib->assembly : klass->image->assembly);
			if (!image)
			{
				return false;
			}
		}
		for (uint16_t i = 0; i < klass->method_count; i++)
		{
			const MethodInfo* methodInfo = klass->methods[i];
			PreJitMethod0(methodInfo);
		}
		return true;
	}

	int32_t PreJitMethod0(const MethodInfo* methodInfo)
	{
		if (!methodInfo->isInterpterImpl)
		{
			return false;
		}
		if (methodInfo->klass->is_generic)
		{
			return false;
		}
		if (!methodInfo->is_inflated)
		{
			if (methodInfo->is_generic)
			{
				return false;
			}
		}
		else
		{
			const Il2CppGenericMethod* genericMethod = methodInfo->genericMethod;
			if (metadata::HasNotInstantiatedGenericType(genericMethod->context.class_inst) || metadata::HasNotInstantiatedGenericType(genericMethod->context.method_inst))
			{
				return false;
			}
		}

		return interpreter::InterpreterModule::GetInterpMethodInfo(methodInfo) != nullptr;
	}

	int32_t RuntimeApi::PreJitMethod(Il2CppReflectionMethod* method)
	{
		return PreJitMethod0(method->method);
	}
}
