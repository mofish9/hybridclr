#pragma once

#include "../CommonDef.h"

namespace hybridclr
{
namespace interpreter
{

#if __has_include("../lab/InstrumentationConfig.h")
#include "../lab/InstrumentationConfig.h"
#endif

#if defined(HYBRIDCLR_LAB_INSTRUMENTED)

	class InterpreterProfile
	{
	public:
		static void Reset();
		static void RecordInterpreterEntry();
		static void RecordDispatch(uint16_t opcode);
		static void RecordTransform(uint64_t nanoseconds);
		static Il2CppString* Snapshot();
	};

#else

	class InterpreterProfile
	{
	public:
		static void Reset() {}
		static void RecordInterpreterEntry() {}
		static void RecordDispatch(uint16_t) {}
		static void RecordTransform(uint64_t) {}
		static Il2CppString* Snapshot() { return nullptr; }
	};

#endif

	class FullGenericSharingDiagnostics
	{
	public:
#if defined(HYBRIDCLR_LAB_FGS_TESTS)
		static void Reset();
		static void RecordDispatch();
		static void RecordInterpreterInvoker();
		static int64_t GetDispatchCount();
		static int64_t GetInterpreterInvokerCount();
#else
		static void Reset() {}
		static void RecordDispatch() {}
		static void RecordInterpreterInvoker() {}
		static int64_t GetDispatchCount() { return 0; }
		static int64_t GetInterpreterInvokerCount() { return 0; }
#endif
	};

}
}
