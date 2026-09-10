#pragma once

#include <set>
#include "../metadata/MetadataDef.h"
#include "../metadata/MetadataUtil.h"

namespace hybridclr
{
namespace transform
{
    // Unity 2022 RuntimeFieldInfo's null/static/receiver validation prefix.
    // Tokens vary with stripping. The caller must resolve and validate all five
    // method identities; no offset alone authorizes execution adaptation.
    template<class MatchCall>
    bool HasFrozenFieldValidationPrefix(const metadata::MethodBody& body,
        const std::set<uint32_t>& splitOffsets, MatchCall matchCall)
    {
        if (!body.ilcodes || body.codeSize <= 80 || !body.exceptionClauses.empty()) return false;
        const uint8_t* il = body.ilcodes;
        const uint8_t positions[] = { 0, 1, 6, 7, 8, 9, 10, 11, 16, 21, 22, 23, 28, 29, 34, 39, 40 };
        const uint8_t values[] = { 2, 0x28, 0x2d, 72, 3, 0x2d, 11, 0x72, 0x73, 0x7a, 2, 0x6f, 3, 0x6f, 0x6f, 0x2d, 39 };
        for (size_t index = 0; index < sizeof(positions); ++index)
            if (il[positions[index]] != values[index]) return false;
        for (uint32_t offset : splitOffsets)
            if (offset > 22 && offset < 39) return false;
        const uint8_t operands[] = { 2, 17, 24, 30, 35 };
        for (size_t index = 0; index < sizeof(operands); ++index)
            if (!matchCall(index, static_cast<uint32_t>(metadata::GetI4LittleEndian(il + operands[index])))) return false;
        return true;
    }
}
}
