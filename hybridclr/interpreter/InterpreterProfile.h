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

}
}
