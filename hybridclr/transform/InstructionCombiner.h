#pragma once

#include <cstring>

#include "TemporaryMemoryArena.h"

#include "../interpreter/Instruction.h"

namespace hybridclr
{
namespace transform
{
	inline interpreter::IRCommon* TryCombineLdlocLdc4AddI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second,
		interpreter::IRCommon* third)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || third == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar || second->type != HiOpcodeEnum::LdcVarConst_4 ||
			third->type != HiOpcodeEnum::BinOpVarVarVar_Add_i4)
		{
			return nullptr;
		}
		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(first);
		IRLdcVarConst_4* ldc = static_cast<IRLdcVarConst_4*>(second);
		IRBinOpVarVarVar_Add_i4* add = static_cast<IRBinOpVarVarVar_Add_i4*>(third);
		bool directOrder = add->op1 == load->dst && add->op2 == ldc->dst;
		bool reverseOrder = add->op2 == load->dst && add->op1 == ldc->dst;
		if (directOrder == reverseOrder || load->src == ldc->dst)
		{
			return nullptr;
		}
		IRLdcVarConst_4_Add_i4* combined = pool.AllocIR<IRLdcVarConst_4_Add_i4>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_Add_i4;
		combined->ret = add->ret;
		combined->op = load->src;
		combined->constant = ldc->src;
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdlocLdc4AndI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second,
		interpreter::IRCommon* third)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || third == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar || second->type != HiOpcodeEnum::LdcVarConst_4 ||
			third->type != HiOpcodeEnum::BinOpVarVarVar_And_i4)
		{
			return nullptr;
		}
		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(first);
		IRLdcVarConst_4* ldc = static_cast<IRLdcVarConst_4*>(second);
		IRBinOpVarVarVar_And_i4* op = static_cast<IRBinOpVarVarVar_And_i4*>(third);
		bool directOrder = op->op1 == load->dst && op->op2 == ldc->dst;
		bool reverseOrder = op->op2 == load->dst && op->op1 == ldc->dst;
		if (directOrder == reverseOrder || load->src == ldc->dst)
		{
			return nullptr;
		}
		IRLdcVarConst_4_And_i4* combined = pool.AllocIR<IRLdcVarConst_4_And_i4>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_And_i4;
		combined->ret = op->ret;
		combined->op = load->src;
		combined->constant = static_cast<int32_t>(ldc->src);
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdlocLdc4MulI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second,
		interpreter::IRCommon* third)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || third == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar || second->type != HiOpcodeEnum::LdcVarConst_4 ||
			third->type != HiOpcodeEnum::BinOpVarVarVar_Mul_i4)
		{
			return nullptr;
		}
		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(first);
		IRLdcVarConst_4* ldc = static_cast<IRLdcVarConst_4*>(second);
		IRBinOpVarVarVar_Mul_i4* mul = static_cast<IRBinOpVarVarVar_Mul_i4*>(third);
		bool directOrder = mul->op1 == load->dst && mul->op2 == ldc->dst;
		bool reverseOrder = mul->op2 == load->dst && mul->op1 == ldc->dst;
		if (directOrder == reverseOrder || load->src == ldc->dst)
		{
			return nullptr;
		}
		IRLdcVarConst_4_Mul_i4* combined = pool.AllocIR<IRLdcVarConst_4_Mul_i4>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_Mul_i4;
		combined->ret = mul->ret;
		combined->op = load->src;
		combined->constant = static_cast<int32_t>(ldc->src);
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdlocLdc4ShrI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second,
		interpreter::IRCommon* third)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || third == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar || second->type != HiOpcodeEnum::LdcVarConst_4 ||
			third->type != HiOpcodeEnum::BitShiftBinOpVarVarVar_Shr_i4_i4)
		{
			return nullptr;
		}
		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(first);
		IRLdcVarConst_4* ldc = static_cast<IRLdcVarConst_4*>(second);
		IRBitShiftBinOpVarVarVar_Shr_i4_i4* shift =
			static_cast<IRBitShiftBinOpVarVarVar_Shr_i4_i4*>(third);
		if (shift->value != load->dst || shift->shiftAmount != ldc->dst ||
			shift->value == ldc->dst || load->src == ldc->dst)
		{
			return nullptr;
		}
		IRLdcVarConst_4_Shr_i4_i4* combined = pool.AllocIR<IRLdcVarConst_4_Shr_i4_i4>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_Shr_i4_i4;
		combined->ret = shift->ret;
		combined->value = load->src;
		combined->shiftAmount = static_cast<int32_t>(ldc->src & 0x1f);
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdlocLdc8MulF8(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second,
		interpreter::IRCommon* third)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || third == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar || second->type != HiOpcodeEnum::LdcVarConst_8 ||
			third->type != HiOpcodeEnum::BinOpVarVarVar_Mul_f8)
		{
			return nullptr;
		}
		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(first);
		IRLdcVarConst_8* ldc = static_cast<IRLdcVarConst_8*>(second);
		IRBinOpVarVarVar_Mul_f8* mul = static_cast<IRBinOpVarVarVar_Mul_f8*>(third);
		bool directOrder = mul->op1 == load->dst && mul->op2 == ldc->dst;
		bool reverseOrder = mul->op2 == load->dst && mul->op1 == ldc->dst;
		if (directOrder == reverseOrder || load->src == ldc->dst)
		{
			return nullptr;
		}
		IRLdcVarConst_8_Mul_f8* combined = pool.AllocIR<IRLdcVarConst_8_Mul_f8>();
		combined->type = HiOpcodeEnum::LdcVarConst_8_Mul_f8;
		combined->ret = mul->ret;
		combined->op = load->src;
		std::memcpy(&combined->constant, &ldc->src, sizeof(combined->constant));
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdlocVarVarPair(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar || second->type != HiOpcodeEnum::LdlocVarVar)
		{
			return nullptr;
		}

		IRLdlocVarVar* firstLdloc = static_cast<IRLdlocVarVar*>(first);
		IRLdlocVarVar* secondLdloc = static_cast<IRLdlocVarVar*>(second);
		IRLdlocVarVar_2* combined = pool.AllocIR<IRLdlocVarVar_2>();
		combined->type = HiOpcodeEnum::LdlocVarVar_2;
		combined->dst0 = firstLdloc->dst;
		combined->src0 = firstLdloc->src;
		combined->dst1 = secondLdloc->dst;
		combined->src1 = secondLdloc->src;
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdlocPairLoad(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar_2 || second->type != HiOpcodeEnum::LdlocVarVar)
		{
			return nullptr;
		}
		IRLdlocVarVar_2* pair = static_cast<IRLdlocVarVar_2*>(first);
		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(second);
		IRLdlocVarVar_3* combined = pool.AllocIR<IRLdlocVarVar_3>();
		combined->type = HiOpcodeEnum::LdlocVarVar_3;
		combined->dst0 = pair->dst0;
		combined->src0 = pair->src0;
		combined->dst1 = pair->dst1;
		combined->src1 = pair->src1;
		combined->dst2 = load->dst;
		combined->src2 = load->src;
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdlocTripleLoad(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar_3 || second->type != HiOpcodeEnum::LdlocVarVar)
		{
			return nullptr;
		}
		IRLdlocVarVar_3* triple = static_cast<IRLdlocVarVar_3*>(first);
		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(second);
		IRLdlocVarVar_4* combined = pool.AllocIR<IRLdlocVarVar_4>();
		combined->type = HiOpcodeEnum::LdlocVarVar_4;
		combined->dst0 = triple->dst0;
		combined->src0 = triple->src0;
		combined->dst1 = triple->dst1;
		combined->src1 = triple->src1;
		combined->dst2 = triple->dst2;
		combined->src2 = triple->src2;
		combined->dst3 = load->dst;
		combined->src3 = load->src;
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdlocPairLdc4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar_2 || second->type != HiOpcodeEnum::LdcVarConst_4)
		{
			return nullptr;
		}
		IRLdlocVarVar_2* pair = static_cast<IRLdlocVarVar_2*>(first);
		IRLdcVarConst_4* ldc = static_cast<IRLdcVarConst_4*>(second);
		IRLdlocVarVar_2_LdcVarConst_4* combined = pool.AllocIR<IRLdlocVarVar_2_LdcVarConst_4>();
		combined->type = HiOpcodeEnum::LdlocVarVar_2_LdcVarConst_4;
		combined->dst0 = pair->dst0;
		combined->src0 = pair->src0;
		combined->dst1 = pair->dst1;
		combined->src1 = pair->src1;
		combined->ldcDst = ldc->dst;
		combined->constant = ldc->src;
		return combined;
	}

	inline interpreter::IRCommon* TryReduceLdlocPairLdc4AndI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar_2_LdcVarConst_4 ||
			second->type != HiOpcodeEnum::BinOpVarVarVar_And_i4)
		{
			return nullptr;
		}
		IRLdlocVarVar_2_LdcVarConst_4* prefix = static_cast<IRLdlocVarVar_2_LdcVarConst_4*>(first);
		IRBinOpVarVarVar_And_i4* op = static_cast<IRBinOpVarVarVar_And_i4*>(second);
		bool constantIsOp1 = op->op1 == prefix->ldcDst;
		bool constantIsOp2 = op->op2 == prefix->ldcDst;
		if (constantIsOp1 == constantIsOp2)
		{
			return nullptr;
		}
		uint16_t valueOperand = constantIsOp1 ? op->op2 : op->op1;
		if (valueOperand != prefix->dst1 || op->ret == prefix->dst0 || prefix->ldcDst == prefix->dst0 ||
			prefix->dst0 == prefix->dst1 || prefix->src0 == prefix->dst1)
		{
			return nullptr;
		}
		uint16_t remainingDst = prefix->dst0;
		uint16_t remainingSrc = prefix->src0;
		uint16_t valueSource = prefix->src1 == prefix->dst0 ? prefix->src0 : prefix->src1;
		if (prefix->ldcDst == valueSource)
		{
			return nullptr;
		}
		int32_t constant = static_cast<int32_t>(prefix->constant);
		prefix->type = HiOpcodeEnum::LdlocVarVar;
		prefix->dst0 = remainingDst;
		prefix->src0 = remainingSrc;

		IRLdcVarConst_4_And_i4* combined = pool.AllocIR<IRLdcVarConst_4_And_i4>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_And_i4;
		combined->ret = op->ret;
		combined->op = valueSource;
		combined->constant = constant;
		return combined;
	}

	inline interpreter::IRCommon* TryReduceLdlocPairLdc4AddI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar_2_LdcVarConst_4 ||
			second->type != HiOpcodeEnum::BinOpVarVarVar_Add_i4)
		{
			return nullptr;
		}
		IRLdlocVarVar_2_LdcVarConst_4* prefix = static_cast<IRLdlocVarVar_2_LdcVarConst_4*>(first);
		IRBinOpVarVarVar_Add_i4* add = static_cast<IRBinOpVarVarVar_Add_i4*>(second);
		bool constantIsOp1 = add->op1 == prefix->ldcDst;
		bool constantIsOp2 = add->op2 == prefix->ldcDst;
		if (constantIsOp1 == constantIsOp2)
		{
			return nullptr;
		}
		uint16_t valueOperand = constantIsOp1 ? add->op2 : add->op1;
		if (valueOperand != prefix->dst1 || add->ret == prefix->dst0 || prefix->ldcDst == prefix->dst0 ||
			prefix->dst0 == prefix->dst1 || prefix->src0 == prefix->dst1)
		{
			return nullptr;
		}
		uint16_t valueSource = prefix->src1 == prefix->dst0 ? prefix->src0 : prefix->src1;
		if (prefix->ldcDst == valueSource)
		{
			return nullptr;
		}
		prefix->type = HiOpcodeEnum::LdlocVarVar;

		IRLdcVarConst_4_Add_i4* combined = pool.AllocIR<IRLdcVarConst_4_Add_i4>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_Add_i4;
		combined->ret = add->ret;
		combined->op = valueSource;
		combined->constant = prefix->constant;
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdc4AddLoad(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdcVarConst_4_Add_i4 || second->type != HiOpcodeEnum::LdlocVarVar)
		{
			return nullptr;
		}
		IRLdcVarConst_4_Add_i4* add = static_cast<IRLdcVarConst_4_Add_i4*>(first);
		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(second);
		if (load->src != add->ret)
		{
			return nullptr;
		}
		IRLdcVarConst_4_Add_i4_LdlocVarVar* combined = pool.AllocIR<IRLdcVarConst_4_Add_i4_LdlocVarVar>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_Add_i4_LdlocVarVar;
		combined->ret = add->ret;
		combined->op = add->op;
		combined->constant = static_cast<int32_t>(add->constant);
		combined->loadDst = load->dst;
		combined->loadSrc = load->src;
		return combined;
	}

	inline interpreter::IRCommon* TryCombineConvertAddLoadPair(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second,
		interpreter::IRCommon* third)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || third == nullptr ||
			first->type != HiOpcodeEnum::ConvertVarVar_i4_i8_Add_i8 ||
			second->type != HiOpcodeEnum::LdlocVarVar || third->type != HiOpcodeEnum::LdlocVarVar)
		{
			return nullptr;
		}
		IRConvertVarVar_i4_i8_Add_i8* add = static_cast<IRConvertVarVar_i4_i8_Add_i8*>(first);
		IRLdlocVarVar* load0 = static_cast<IRLdlocVarVar*>(second);
		IRLdlocVarVar* load1 = static_cast<IRLdlocVarVar*>(third);
		IRConvertVarVar_i4_i8_Add_i8_LdlocVarVar_2* combined =
			pool.AllocIR<IRConvertVarVar_i4_i8_Add_i8_LdlocVarVar_2>();
		combined->type = HiOpcodeEnum::ConvertVarVar_i4_i8_Add_i8_LdlocVarVar_2;
		combined->ret = add->ret;
		combined->converted = add->converted;
		combined->other = add->other;
		combined->dst0 = load0->dst;
		combined->src0 = load0->src;
		combined->dst1 = load1->dst;
		combined->src1 = load1->src;
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdc4AddLoadPair(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second,
		interpreter::IRCommon* third)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || third == nullptr ||
			first->type != HiOpcodeEnum::LdcVarConst_4_Add_i4 ||
			second->type != HiOpcodeEnum::LdlocVarVar || third->type != HiOpcodeEnum::LdlocVarVar)
		{
			return nullptr;
		}
		IRLdcVarConst_4_Add_i4* add = static_cast<IRLdcVarConst_4_Add_i4*>(first);
		IRLdlocVarVar* load0 = static_cast<IRLdlocVarVar*>(second);
		IRLdlocVarVar* load1 = static_cast<IRLdlocVarVar*>(third);
		IRLdcVarConst_4_Add_i4_LdlocVarVar_2* combined = pool.AllocIR<IRLdcVarConst_4_Add_i4_LdlocVarVar_2>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_Add_i4_LdlocVarVar_2;
		combined->ret = add->ret;
		combined->op = add->op;
		combined->constant = static_cast<int32_t>(add->constant);
		combined->dst0 = load0->dst;
		combined->src0 = load0->src;
		combined->dst1 = load1->dst;
		combined->src1 = load1->src;
		return combined;
	}

	template<typename TBinOp>
	inline interpreter::IRCommon* TryCombineLdc4I4BinOp(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second,
		interpreter::HiOpcodeEnum op, interpreter::HiOpcodeEnum combinedOp)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || first->type != HiOpcodeEnum::LdcVarConst_4 || second->type != op)
		{
			return nullptr;
		}
		IRLdcVarConst_4* ldc = static_cast<IRLdcVarConst_4*>(first);
		TBinOp* bin = static_cast<TBinOp*>(second);
		if ((bin->op1 == ldc->dst) == (bin->op2 == ldc->dst))
		{
			return nullptr;
		}
		uint16_t other = bin->op1 == ldc->dst ? bin->op2 : bin->op1;
		if (combinedOp == HiOpcodeEnum::LdcVarConst_4_And_i4)
		{
			IRLdcVarConst_4_And_i4* combined = pool.AllocIR<IRLdcVarConst_4_And_i4>();
			combined->type = combinedOp;
			combined->ret = bin->ret;
			combined->op = other;
			combined->constant = static_cast<int32_t>(ldc->src);
			return combined;
		}
		IRLdcVarConst_4_Mul_i4* combined = pool.AllocIR<IRLdcVarConst_4_Mul_i4>();
		combined->type = combinedOp;
		combined->ret = bin->ret;
		combined->op = other;
		combined->constant = static_cast<int32_t>(ldc->src);
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdc8MulF8(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdcVarConst_8 || second->type != HiOpcodeEnum::BinOpVarVarVar_Mul_f8)
		{
			return nullptr;
		}
		IRLdcVarConst_8* ldc = static_cast<IRLdcVarConst_8*>(first);
		IRBinOpVarVarVar_Mul_f8* mul = static_cast<IRBinOpVarVarVar_Mul_f8*>(second);
		if ((mul->op1 == ldc->dst) == (mul->op2 == ldc->dst))
		{
			return nullptr;
		}
		IRLdcVarConst_8_Mul_f8* combined = pool.AllocIR<IRLdcVarConst_8_Mul_f8>();
		combined->type = HiOpcodeEnum::LdcVarConst_8_Mul_f8;
		combined->ret = mul->ret;
		combined->op = mul->op1 == ldc->dst ? mul->op2 : mul->op1;
		std::memcpy(&combined->constant, &ldc->src, sizeof(combined->constant));
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdc4ShrI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdcVarConst_4 || second->type != HiOpcodeEnum::BitShiftBinOpVarVarVar_Shr_i4_i4)
		{
			return nullptr;
		}
		IRLdcVarConst_4* ldc = static_cast<IRLdcVarConst_4*>(first);
		IRBitShiftBinOpVarVarVar_Shr_i4_i4* shift = static_cast<IRBitShiftBinOpVarVarVar_Shr_i4_i4*>(second);
		if (shift->shiftAmount != ldc->dst || shift->value == ldc->dst)
		{
			return nullptr;
		}
		IRLdcVarConst_4_Shr_i4_i4* combined = pool.AllocIR<IRLdcVarConst_4_Shr_i4_i4>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_Shr_i4_i4;
		combined->ret = shift->ret;
		combined->value = shift->value;
		combined->shiftAmount = static_cast<int32_t>(ldc->src & 0x1f);
		return combined;
	}

	inline bool TryRedirectStlocDestination(interpreter::IRCommon* producer,
		uint16_t stackSource, uint16_t localDestination)
	{
		using namespace interpreter;
		if (producer == nullptr)
		{
			return false;
		}

#define REDIRECT_DESTINATION(opcode, structType, field) \
		case HiOpcodeEnum::opcode: \
		{ \
			structType* typed = static_cast<structType*>(producer); \
			if (typed->field != stackSource) return false; \
			typed->field = localDestination; \
			return true; \
		}

		switch (producer->type)
		{
			REDIRECT_DESTINATION(LdlocVarVar, IRLdlocVarVar, dst)
			REDIRECT_DESTINATION(LdcVarConst_4, IRLdcVarConst_4, dst)
			REDIRECT_DESTINATION(LdcVarConst_8, IRLdcVarConst_8, dst)
			REDIRECT_DESTINATION(BinOpVarVarVar_Add_i4, IRBinOpVarVarVar_Add_i4, ret)
			REDIRECT_DESTINATION(BinOpVarVarVar_Add_i8, IRBinOpVarVarVar_Add_i8, ret)
			REDIRECT_DESTINATION(BinOpVarVarVar_Mul_i4, IRBinOpVarVarVar_Mul_i4, ret)
			REDIRECT_DESTINATION(BinOpVarVarVar_Mul_f8, IRBinOpVarVarVar_Mul_f8, ret)
			REDIRECT_DESTINATION(BinOpVarVarVar_And_i4, IRBinOpVarVarVar_And_i4, ret)
			REDIRECT_DESTINATION(BinOpVarVarVar_Xor_i4, IRBinOpVarVarVar_Xor_i4, ret)
			REDIRECT_DESTINATION(BitShiftBinOpVarVarVar_Shr_i4_i4, IRBitShiftBinOpVarVarVar_Shr_i4_i4, ret)
			REDIRECT_DESTINATION(ConvertVarVar_i4_i8, IRConvertVarVar_i4_i8, dst)
			REDIRECT_DESTINATION(ConvertVarVar_i4_f8, IRConvertVarVar_i4_f8, dst)
			REDIRECT_DESTINATION(LdcVarConst_4_Add_i4, IRLdcVarConst_4_Add_i4, ret)
			REDIRECT_DESTINATION(ConvertVarVar_i4_i8_Add_i8, IRConvertVarVar_i4_i8_Add_i8, ret)
			REDIRECT_DESTINATION(LdcVarConst_4_And_i4, IRLdcVarConst_4_And_i4, ret)
			REDIRECT_DESTINATION(LdcVarConst_4_Mul_i4, IRLdcVarConst_4_Mul_i4, ret)
			REDIRECT_DESTINATION(LdcVarConst_8_Mul_f8, IRLdcVarConst_8_Mul_f8, ret)
			REDIRECT_DESTINATION(LdcVarConst_4_Shr_i4_i4, IRLdcVarConst_4_Shr_i4_i4, ret)
		case HiOpcodeEnum::LdlocVarVar_2:
		{
			IRLdlocVarVar_2* loads = static_cast<IRLdlocVarVar_2*>(producer);
			if (loads->dst1 != stackSource) return false;
			loads->dst1 = localDestination;
			return true;
		}
		case HiOpcodeEnum::LdlocVarVar_3:
		{
			IRLdlocVarVar_3* loads = static_cast<IRLdlocVarVar_3*>(producer);
			if (loads->dst2 != stackSource) return false;
			loads->dst2 = localDestination;
			return true;
		}
		case HiOpcodeEnum::LdlocVarVar_4:
		{
			IRLdlocVarVar_4* loads = static_cast<IRLdlocVarVar_4*>(producer);
			if (loads->dst3 != stackSource) return false;
			loads->dst3 = localDestination;
			return true;
		}
		default:
			return false;
		}
#undef REDIRECT_DESTINATION
	}

	inline interpreter::IRCommon* TryPropagateLdlocToConsumer(interpreter::IRCommon* first,
		interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || first->type != HiOpcodeEnum::LdlocVarVar)
		{
			return nullptr;
		}
		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(first);

#define PROPAGATE_BINARY(opcode, structType) \
		case HiOpcodeEnum::opcode: \
		{ \
			structType* consumer = static_cast<structType*>(second); \
			bool used = consumer->op1 == load->dst || consumer->op2 == load->dst; \
			if (!used) return nullptr; \
			if (consumer->op1 == load->dst) consumer->op1 = load->src; \
			if (consumer->op2 == load->dst) consumer->op2 = load->src; \
			return consumer; \
		}
#define PROPAGATE_BRANCH(opcode, structType) PROPAGATE_BINARY(opcode, structType)

		switch (second->type)
		{
			PROPAGATE_BINARY(BinOpVarVarVar_Add_i4, IRBinOpVarVarVar_Add_i4)
			PROPAGATE_BINARY(BinOpVarVarVar_Add_i8, IRBinOpVarVarVar_Add_i8)
			PROPAGATE_BINARY(BinOpVarVarVar_Mul_i4, IRBinOpVarVarVar_Mul_i4)
			PROPAGATE_BINARY(BinOpVarVarVar_Mul_f8, IRBinOpVarVarVar_Mul_f8)
			PROPAGATE_BINARY(BinOpVarVarVar_And_i4, IRBinOpVarVarVar_And_i4)
			PROPAGATE_BINARY(BinOpVarVarVar_Xor_i4, IRBinOpVarVarVar_Xor_i4)
			PROPAGATE_BRANCH(BranchVarVar_Clt_i4, IRBranchVarVar_Clt_i4)
		case HiOpcodeEnum::LdcVarConst_4_Add_i4:
		{
			IRLdcVarConst_4_Add_i4* consumer = static_cast<IRLdcVarConst_4_Add_i4*>(second);
			if (consumer->op != load->dst) return nullptr;
			consumer->op = load->src;
			return consumer;
		}
		case HiOpcodeEnum::ConvertVarVar_i4_i8_Add_i8:
		{
			IRConvertVarVar_i4_i8_Add_i8* consumer = static_cast<IRConvertVarVar_i4_i8_Add_i8*>(second);
			bool used = consumer->converted == load->dst || consumer->other == load->dst;
			if (!used) return nullptr;
			if (consumer->converted == load->dst) consumer->converted = load->src;
			if (consumer->other == load->dst) consumer->other = load->src;
			return consumer;
		}
		case HiOpcodeEnum::LdcVarConst_4_And_i4:
		{
			IRLdcVarConst_4_And_i4* consumer = static_cast<IRLdcVarConst_4_And_i4*>(second);
			if (consumer->op != load->dst) return nullptr;
			consumer->op = load->src;
			return consumer;
		}
		case HiOpcodeEnum::LdcVarConst_4_Mul_i4:
		{
			IRLdcVarConst_4_Mul_i4* consumer = static_cast<IRLdcVarConst_4_Mul_i4*>(second);
			if (consumer->op != load->dst) return nullptr;
			consumer->op = load->src;
			return consumer;
		}
		case HiOpcodeEnum::LdcVarConst_8_Mul_f8:
		{
			IRLdcVarConst_8_Mul_f8* consumer = static_cast<IRLdcVarConst_8_Mul_f8*>(second);
			if (consumer->op != load->dst) return nullptr;
			consumer->op = load->src;
			return consumer;
		}
		case HiOpcodeEnum::LdcVarConst_4_Shr_i4_i4:
		{
			IRLdcVarConst_4_Shr_i4_i4* consumer = static_cast<IRLdcVarConst_4_Shr_i4_i4*>(second);
			if (consumer->value != load->dst) return nullptr;
			consumer->value = load->src;
			return consumer;
		}
		case HiOpcodeEnum::LdfldVarVar_i4:
		{
			IRLdfldVarVar_i4* consumer = static_cast<IRLdfldVarVar_i4*>(second);
			if (consumer->obj != load->dst) return nullptr;
			consumer->obj = load->src;
			return consumer;
		}
		case HiOpcodeEnum::BranchSwitch:
		{
			IRBranchSwitch* consumer = static_cast<IRBranchSwitch*>(second);
			if (consumer->value != load->dst) return nullptr;
			consumer->value = load->src;
			return consumer;
		}
		case HiOpcodeEnum::StfldVarVar_i4:
		{
			IRStfldVarVar_i4* consumer = static_cast<IRStfldVarVar_i4*>(second);
			bool used = consumer->obj == load->dst || consumer->data == load->dst;
			if (!used) return nullptr;
			if (consumer->obj == load->dst) consumer->obj = load->src;
			if (consumer->data == load->dst) consumer->data = load->src;
			return consumer;
		}
		default:
			return nullptr;
		}
#undef PROPAGATE_BRANCH
#undef PROPAGATE_BINARY
	}

	inline interpreter::IRCommon* TryPropagateLdlocPairToStfldI4(interpreter::IRCommon* first,
		interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr || first->type != HiOpcodeEnum::LdlocVarVar_2 ||
			second->type != HiOpcodeEnum::StfldVarVar_i4)
		{
			return nullptr;
		}
		IRLdlocVarVar_2* loads = static_cast<IRLdlocVarVar_2*>(first);
		IRStfldVarVar_i4* store = static_cast<IRStfldVarVar_i4*>(second);
		if (loads->dst0 == loads->dst1 || loads->src0 == loads->dst1)
		{
			return nullptr;
		}
		bool usesDst0 = store->obj == loads->dst0 || store->data == loads->dst0;
		bool usesDst1 = store->obj == loads->dst1 || store->data == loads->dst1;
		if (!usesDst0 || !usesDst1)
		{
			return nullptr;
		}
		uint16_t secondSource = loads->src1 == loads->dst0 ? loads->src0 : loads->src1;
		auto ResolveSource = [loads, secondSource](uint16_t operand)
		{
			if (operand == loads->dst1) return secondSource;
			return operand == loads->dst0 ? loads->src0 : operand;
		};
		store->obj = ResolveSource(store->obj);
		store->data = ResolveSource(store->data);
		return store;
	}

	inline bool TryReduceLdlocTripleBeforeGetArrayI4(interpreter::IRCommon* first,
		interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar_3 || second->type != HiOpcodeEnum::GetArrayElementVarVar_i4)
		{
			return false;
		}
		IRLdlocVarVar_3* loads = static_cast<IRLdlocVarVar_3*>(first);
		IRGetArrayElementVarVar_i4* get = static_cast<IRGetArrayElementVarVar_i4*>(second);
		if (loads->dst0 == loads->dst1 || loads->dst0 == loads->dst2 || loads->dst1 == loads->dst2 ||
			loads->src0 == loads->dst1 || loads->src0 == loads->dst2 || loads->src1 == loads->dst2)
		{
			return false;
		}
		if (get->arr != loads->dst1 || get->index != loads->dst2)
		{
			return false;
		}

		uint16_t arraySource = loads->src1 == loads->dst0 ? loads->src0 : loads->src1;
		uint16_t indexSource = loads->src2;
		if (indexSource == loads->dst1)
		{
			indexSource = arraySource;
		}
		else if (indexSource == loads->dst0)
		{
			indexSource = loads->src0;
		}

		uint16_t remainingDst = loads->dst0;
		uint16_t remainingSrc = loads->src0;
		loads->type = HiOpcodeEnum::LdlocVarVar;
		loads->dst0 = remainingDst;
		loads->src0 = remainingSrc;
		get->arr = arraySource;
		get->index = indexSource;
		return true;
	}

	inline interpreter::IRCommon* TryCombineLdc4AddI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdcVarConst_4 || second->type != HiOpcodeEnum::BinOpVarVarVar_Add_i4)
		{
			return nullptr;
		}

		IRLdcVarConst_4* ldc = static_cast<IRLdcVarConst_4*>(first);
		IRBinOpVarVarVar_Add_i4* add = static_cast<IRBinOpVarVarVar_Add_i4*>(second);
		if (add->op1 != ldc->dst && add->op2 != ldc->dst)
		{
			return nullptr;
		}
		if (add->op1 == ldc->dst && add->op2 == ldc->dst)
		{
			return nullptr;
		}

		IRLdcVarConst_4_Add_i4* combined = pool.AllocIR<IRLdcVarConst_4_Add_i4>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_Add_i4;
		combined->ret = add->ret;
		combined->op = add->op1 == ldc->dst ? add->op2 : add->op1;
		combined->constant = ldc->src;
		return combined;
	}

	inline interpreter::IRCommon* TryCombineLdc4AddI4Ret4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdcVarConst_4_Add_i4 || second->type != HiOpcodeEnum::RetVar_ret_4)
		{
			return nullptr;
		}
		IRLdcVarConst_4_Add_i4* add = static_cast<IRLdcVarConst_4_Add_i4*>(first);
		IRRetVar_ret_4* ret = static_cast<IRRetVar_ret_4*>(second);
		if (add->ret != ret->ret)
		{
			return nullptr;
		}
		IRLdcVarConst_4_Add_i4_Ret_4* combined = pool.AllocIR<IRLdcVarConst_4_Add_i4_Ret_4>();
		combined->type = HiOpcodeEnum::LdcVarConst_4_Add_i4_Ret_4;
		combined->ret = add->ret;
		combined->op = add->op;
		combined->constant = add->constant;
		return combined;
	}

	inline interpreter::IRCommon* TryCombineConvertI4I8AddI8(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::ConvertVarVar_i4_i8 || second->type != HiOpcodeEnum::BinOpVarVarVar_Add_i8)
		{
			return nullptr;
		}

		IRConvertVarVar_i4_i8* convert = static_cast<IRConvertVarVar_i4_i8*>(first);
		IRBinOpVarVarVar_Add_i8* add = static_cast<IRBinOpVarVarVar_Add_i8*>(second);
		if (convert->dst != convert->src || (add->op1 != convert->dst && add->op2 != convert->dst))
		{
			return nullptr;
		}
		if (add->op1 == convert->dst && add->op2 == convert->dst)
		{
			return nullptr;
		}

		IRConvertVarVar_i4_i8_Add_i8* combined = pool.AllocIR<IRConvertVarVar_i4_i8_Add_i8>();
		combined->type = HiOpcodeEnum::ConvertVarVar_i4_i8_Add_i8;
		combined->ret = add->ret;
		combined->converted = convert->src;
		combined->other = add->op1 == convert->dst ? add->op2 : add->op1;
		return combined;
	}

	inline interpreter::IRCommon* TryPropagateLdlocToBranchCltI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		(void)pool;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar || second->type != HiOpcodeEnum::BranchVarVar_Clt_i4)
		{
			return nullptr;
		}

		IRLdlocVarVar* load = static_cast<IRLdlocVarVar*>(first);
		IRBranchVarVar_Clt_i4* branch = static_cast<IRBranchVarVar_Clt_i4*>(second);
		if (branch->op1 != load->dst && branch->op2 != load->dst)
		{
			return nullptr;
		}

		if (branch->op1 == load->dst)
		{
			branch->op1 = load->src;
		}
		if (branch->op2 == load->dst)
		{
			branch->op2 = load->src;
		}
		return branch;
	}

	inline interpreter::IRCommon* TryPropagateLdlocPairToBranchCltI4(TemporaryMemoryArena& pool,
		interpreter::IRCommon* first, interpreter::IRCommon* second)
	{
		using namespace interpreter;
		(void)pool;
		if (first == nullptr || second == nullptr ||
			first->type != HiOpcodeEnum::LdlocVarVar_2 || second->type != HiOpcodeEnum::BranchVarVar_Clt_i4)
		{
			return nullptr;
		}

		IRLdlocVarVar_2* loads = static_cast<IRLdlocVarVar_2*>(first);
		IRBranchVarVar_Clt_i4* branch = static_cast<IRBranchVarVar_Clt_i4*>(second);
		if (loads->dst0 == loads->dst1 || loads->src0 == loads->dst1)
		{
			return nullptr;
		}
		bool usesDst0 = branch->op1 == loads->dst0 || branch->op2 == loads->dst0;
		bool usesDst1 = branch->op1 == loads->dst1 || branch->op2 == loads->dst1;
		if (!usesDst0 || !usesDst1)
		{
			return nullptr;
		}

		uint16_t secondSource = loads->src1 == loads->dst0 ? loads->src0 : loads->src1;
		auto ResolveSource = [loads, secondSource](uint16_t operand)
		{
			if (operand == loads->dst1)
			{
				return secondSource;
			}
			return operand == loads->dst0 ? loads->src0 : operand;
		};

		branch->op1 = ResolveSource(branch->op1);
		branch->op2 = ResolveSource(branch->op2);
		return branch;
	}
}
}
