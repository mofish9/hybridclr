#pragma once

#include "os/ThreadLocalValue.h"

#include "../CommonDef.h"
#include "MethodBridge.h"
#include "Engine.h"
#include "InterpreterProfile.h"
#include "../metadata/Image.h"

namespace hybridclr
{
namespace interpreter
{

	class InterpreterModule
	{
	public:
		static void Initialize();

		static MachineState& GetCurrentThreadMachineState()
		{
			MachineState* state = nullptr;
			s_machineState.GetValue((void**)&state);
			if (!state)
			{
				state = new MachineState();
				s_machineState.SetValue(state);
			}
			return *state;
		}

		static void FreeThreadLocalMachineState()
		{
			MachineState* state = nullptr;
			s_machineState.GetValue((void**)&state);
			if (state)
			{
				delete state;
				s_machineState.SetValue(nullptr);
			}
		}

		static InterpMethodInfo* GetInterpMethodInfo(const MethodInfo* methodInfo);
		// The caller may already hold g_MetadataLock while preparing a batch of
		// methods. This avoids one lock transition per method without changing the
		// default thread-safe path used by normal first execution.
		static InterpMethodInfo* GetInterpMethodInfo(const MethodInfo* methodInfo, bool metadataLockHeld);

		static Il2CppMethodPointer GetMethodPointer(const Il2CppMethodDefinition* method);
		static Il2CppMethodPointer GetMethodPointer(const MethodInfo* method);
		static Il2CppMethodPointer GetAdjustThunkMethodPointer(const Il2CppMethodDefinition* method);
		static Il2CppMethodPointer GetAdjustThunkMethodPointer(const MethodInfo* method);
		static Managed2NativeCallMethod GetManaged2NativeMethodPointer(const MethodInfo* method, bool forceStatic);
		static Managed2NativeCallMethod GetManaged2NativeMethodPointer(const metadata::ResolveStandAloneMethodSig& methodSig);
		static Managed2NativeCallMethod ResolveRuntimeManaged2NativeMethodPointer(
			const MethodInfo* method, Managed2NativeCallMethod fallback)
		{
			if (IsFullGenericSharingMethod(method))
			{
				return Managed2NativeCallByReflectionInvoke;
			}
			return fallback;
		}
		static Managed2NativeFunctionPointerCallMethod GetManaged2NativeFunctionPointerMethodPointer(const MethodInfo* method, Il2CppCallConvention callConvention);
		static Managed2NativeFunctionPointerCallMethod GetManaged2NativeFunctionPointerMethodPointer(const metadata::ResolveStandAloneMethodSig& methodSig);

		static InvokerMethod GetMethodInvoker(const Il2CppMethodDefinition* method);
		static InvokerMethod GetMethodInvoker(const MethodInfo* method);

		static bool IsImplementsByInterpreter(const MethodInfo* method);

		static bool HasImplementCallNative2Managed(const MethodInfo* method)
		{
			Il2CppMethodPointer methodPointer = ReadPublishedPointer(
				&const_cast<MethodInfo*>(method)->methodPointerCallByInterp);
			IL2CPP_ASSERT(methodPointer != NotSupportAdjustorThunk);
			return methodPointer != (Il2CppMethodPointer)NotSupportNative2Managed;
		}

		static bool HasImplementCallVirtualNative2Managed(const MethodInfo* method)
		{
			Il2CppMethodPointer methodPointer = ReadPublishedPointer(
				&const_cast<MethodInfo*>(method)->virtualMethodPointerCallByInterp);
			IL2CPP_ASSERT(methodPointer != NotSupportNative2Managed);
			return methodPointer != (Il2CppMethodPointer)NotSupportAdjustorThunk;
		}

		static void Managed2NativeCallByReflectionInvoke(const MethodInfo* method, uint16_t* argVarIndexs, StackObject* localVarBase, void* ret);

		static void NotSupportNative2Managed();
		static void NotSupportAdjustorThunk();

		static Il2CppMethodPointer GetReversePInvokeWrapper(const Il2CppImage* image, const MethodInfo* method, Il2CppCallConvention callConvention);
		static const MethodInfo* GetMethodInfoByReversePInvokeWrapperIndex(int32_t index);
		static const MethodInfo* GetMethodInfoByReversePInvokeWrapperMethodPointer(Il2CppMethodPointer methodPointer);
		static int32_t GetWrapperIndexByReversePInvokeWrapperMethodPointer(Il2CppMethodPointer methodPointer);

		static const char* GetValueTypeSignature(const char* fullName);
		
		static bool IsMethodInfoPointer(void* pointer);
	private:
		static il2cpp::os::ThreadLocalValue s_machineState;
	};
}
}
