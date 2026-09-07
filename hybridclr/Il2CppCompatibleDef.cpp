#include "Il2CppCompatibleDef.h"

#include <cstring>

#include "vm/Runtime.h"
#include "os/Mutex.h"

#include "metadata/MetadataModule.h"
#include "interpreter/InterpreterModule.h"

namespace hybridclr
{
	namespace
	{
		baselib::ReentrantLock s_methodPointerInitLock;

		struct ResolvedInterpreterCallMethodPointers
		{
			bool implementedByInterpreter = false;
			Il2CppMethodPointer methodPointer = nullptr;
			Il2CppMethodPointer virtualMethodPointer = nullptr;
			InvokerMethod invoker = nullptr;
		};

#if HYBRIDCLR_UNITY_2021_OR_NEW
		const uint32_t kFullGenericSharingPreparationReady = 1;
		const uint32_t kFullGenericSharingPreparationInProgress = UINT32_MAX;
		uint32_t s_aotMetadataVersion = 1;

		uint32_t ReadPublishedUInt32(const uint32_t* value)
		{
			uint32_t result = (uint32_t)il2cpp::os::Atomic::LoadRelaxed((const int32_t*)value);
			Baselib_atomic_thread_fence_acquire();
			return result;
		}

		void PublishUInt32(uint32_t* destination, uint32_t value)
		{
			il2cpp::os::Atomic::Exchange(destination, value);
		}
#endif

		bool HasValidInvoker(const MethodInfo* method)
		{
#if HYBRIDCLR_UNITY_2021_OR_NEW
			if (IsFullGenericSharingMethod(method) &&
				!method->isInterpterImpl &&
				!method->hasFullGenericSharingAotInvoker)
			{
				return false;
			}
#endif
			if (method->invoker_method == nullptr)
			{
				return false;
			}
#if HYBRIDCLR_UNITY_2021_OR_NEW
			return method->invoker_method != il2cpp::vm::Runtime::GetMissingMethodInvoker();
#else
			return true;
#endif
		}

		ResolvedInterpreterCallMethodPointers ResolveInterpreterCallMethodPointers(
			const MethodInfo* method, bool implementedByInterpreter)
		{
			ResolvedInterpreterCallMethodPointers resolved;
			resolved.implementedByInterpreter = implementedByInterpreter;
			if (!implementedByInterpreter)
			{
				return resolved;
			}

			resolved.methodPointer = interpreter::InterpreterModule::GetMethodPointer(method);
			bool isAdjustorThunkMethod = IS_CLASS_VALUE_TYPE(method->klass) &&
				hybridclr::metadata::IsInstanceMethod(method);
			resolved.virtualMethodPointer = isAdjustorThunkMethod
				? interpreter::InterpreterModule::GetAdjustThunkMethodPointer(method)
				: resolved.methodPointer;
			resolved.invoker = interpreter::InterpreterModule::GetMethodInvoker(method);
			return resolved;
		}

		Il2CppMethodPointer InitializeInterpreterCallMethodPointersLocked(MethodInfo* method,
			const ResolvedInterpreterCallMethodPointers& resolved, bool forceInitialization)
		{
			if (!forceInitialization && method->initInterpCallMethodPointer)
			{
				return ReadPublishedPointer(&method->methodPointerCallByInterp);
			}

			// All metadata and signature resolution has completed before this lock is
			// acquired. Only publish the immutable snapshot while holding the lock.
			method->initInterpCallMethodPointer = true;
			if (resolved.implementedByInterpreter)
			{
				if (method->invoker_method == nullptr
#if HYBRIDCLR_UNITY_2021_OR_NEW
				|| method->invoker_method == il2cpp::vm::Runtime::GetMissingMethodInvoker()
				|| IsFullGenericSharingMethod(method)
#endif
				)
				{
					method->invoker_method = resolved.invoker;
				}
#if HYBRIDCLR_UNITY_2021_OR_NEW
				if (method->methodPointer == nullptr || IsFullGenericSharingMethod(method))
				{
					il2cpp::os::Atomic::PublishPointer(&method->methodPointer, resolved.methodPointer);
				}
				if (method->virtualMethodPointer == nullptr || IsFullGenericSharingMethod(method))
				{
					il2cpp::os::Atomic::PublishPointer(&method->virtualMethodPointer, resolved.virtualMethodPointer);
				}
#else
				if (method->methodPointer == nullptr)
				{
					il2cpp::os::Atomic::PublishPointer(&method->methodPointer, resolved.virtualMethodPointer);
				}
#endif
				method->isInterpterImpl = true;
				il2cpp::os::Atomic::PublishPointer(&method->virtualMethodPointerCallByInterp, resolved.virtualMethodPointer);
				// This is the readiness field read by the lock-free fast path. Publish it
				// only after every pointer, invoker, and interpreter flag is initialized.
				il2cpp::os::Atomic::PublishPointer(&method->methodPointerCallByInterp, resolved.methodPointer);
			}
			return ReadPublishedPointer(&method->methodPointerCallByInterp);
		}
	}

	void CopyMethodInfo(MethodInfo* destination, const MethodInfo* source, size_t size)
	{
		il2cpp::os::FastAutoLock lock(&s_methodPointerInitLock);
		std::memcpy(destination, source, size);
#if HYBRIDCLR_UNITY_2021_OR_NEW
		// The execution pointers may be copied, but preparation ownership belongs
		// to the new MethodInfo and must never inherit an in-progress sentinel.
		destination->fullGenericSharingPreparationState = 0;
#endif
	}

	void NotifyAOTMetadataLoaded()
	{
#if HYBRIDCLR_UNITY_2021_OR_NEW
		uint32_t version = il2cpp::os::Atomic::Increment(&s_aotMetadataVersion);
		// Zero is reserved for the initial epoch. Keep registration ordering
		// well-defined if the counter ever wraps after billions of metadata loads.
		if (version == 0)
		{
			il2cpp::os::Atomic::CompareExchange(&s_aotMetadataVersion, 1, 0);
		}
#endif
	}

	Il2CppMethodPointer InitAndGetInterpreterDirectlyCallMethodPointerSlow(MethodInfo* method)
	{
		// MetadataModule::IsImplementedByInterpreter may acquire g_MetadataLock.
		// Resolve it before taking the method-pointer lock so this path never
		// creates an s_methodPointerInitLock -> g_MetadataLock edge.
		bool implementedByInterpreter = hybridclr::metadata::MetadataModule::IsImplementedByInterpreter(
			method, ReadPublishedPointer(&method->methodPointer) == nullptr);
		ResolvedInterpreterCallMethodPointers resolved = ResolveInterpreterCallMethodPointers(
			method, implementedByInterpreter);
		il2cpp::os::FastAutoLock lock(&s_methodPointerInitLock);
		return InitializeInterpreterCallMethodPointersLocked(method, resolved, false);
	}

	bool PrepareFullGenericSharingMethod(const MethodInfo* method)
	{
#if HYBRIDCLR_UNITY_2021_OR_NEW
		for (;;)
		{
			if (ReadPublishedUInt32(&method->fullGenericSharingPreparationState) ==
				kFullGenericSharingPreparationReady)
			{
				return HasValidInvoker(method);
			}

			bool needsInterpreterFallback = !method->hasFullGenericSharingAotInvoker;
			uint32_t metadataVersion = ReadPublishedUInt32(&s_aotMetadataVersion);
			bool implementedByInterpreter = false;
			// This may acquire g_MetadataLock, so it must remain outside the method
			// pointer lock. The version recheck makes the first-use decision linearize
			// before or after a concurrent metadata registration.
			if (needsInterpreterFallback)
			{
				implementedByInterpreter = hybridclr::metadata::MetadataModule::IsImplementedByInterpreter(
					const_cast<MethodInfo*>(method), true);
			}
			ResolvedInterpreterCallMethodPointers resolved;
			if (implementedByInterpreter)
			{
				resolved = ResolveInterpreterCallMethodPointers(method, true);
			}
			il2cpp::os::FastAutoLock lock(&s_methodPointerInitLock);

			uint32_t preparationState = ReadPublishedUInt32(&method->fullGenericSharingPreparationState);
			if (preparationState == kFullGenericSharingPreparationReady)
			{
				return HasValidInvoker(method);
			}
			if (preparationState == kFullGenericSharingPreparationInProgress)
			{
				// The lock is reentrant, so this is a recursive lookup from the thread
				// currently producing the method bridge. The outer call publishes ready.
				return HasValidInvoker(method);
			}
			if (needsInterpreterFallback &&
				ReadPublishedUInt32(&s_aotMetadataVersion) != metadataVersion)
			{
				continue;
			}

			MethodInfo* mutableMethod = const_cast<MethodInfo*>(method);
			PublishUInt32(&mutableMethod->fullGenericSharingPreparationState,
				kFullGenericSharingPreparationInProgress);
			if (implementedByInterpreter &&
				!interpreter::InterpreterModule::IsImplementsByInterpreter(method))
			{
				InitializeInterpreterCallMethodPointersLocked(mutableMethod, resolved, true);
			}
			PublishUInt32(&mutableMethod->fullGenericSharingPreparationState,
				kFullGenericSharingPreparationReady);
			return HasValidInvoker(method);
		}
#else
		return HasValidInvoker(method);
#endif
	}
}
