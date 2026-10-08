#pragma once

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

// Native scalar destination extent for provenance invalidation. Source type
// suffixes are not output widths: BITREPLICATE_B64_B32 writes a pair, while
// BCNT/FF/FLBIT_I32_B64 each write one DWORD. Memory load widths are decoded.
inline uint32_t ScalarDestinationDwords(const Instruction& inst) {
	if (inst.family == Family::VOPC || magic_enum::enum_name(inst.opcode).starts_with("V_CMP"))
		return 2u; // Vector comparisons produce a scalar lane mask, including VOP3.
	switch (inst.opcode) {
		case Opcode::S_MOV_B64:
		case Opcode::S_CMOV_B64:
		case Opcode::S_NOT_B64:
		case Opcode::S_WQM_B64:
		case Opcode::S_BREV_B64:
		case Opcode::S_BITSET0_B64:
		case Opcode::S_BITSET1_B64:
		case Opcode::S_GETPC_B64:
		case Opcode::S_SWAPPC_B64:
		case Opcode::S_AND_SAVEEXEC_B64:
		case Opcode::S_ORN2_SAVEEXEC_B64:
		case Opcode::S_ANDN1_SAVEEXEC_B64:
		case Opcode::S_QUADMASK_B64:
		case Opcode::S_BITREPLICATE_B64_B32:
		case Opcode::S_CSELECT_B64:
		case Opcode::S_AND_B64:
		case Opcode::S_OR_B64:
		case Opcode::S_XOR_B64:
		case Opcode::S_ANDN2_B64:
		case Opcode::S_ORN2_B64:
		case Opcode::S_NAND_B64:
		case Opcode::S_NOR_B64:
		case Opcode::S_XNOR_B64:
		case Opcode::S_LSHL_B64:
		case Opcode::S_LSHR_B64:
		case Opcode::S_ASHR_I64:
		case Opcode::S_BFM_B64:
		case Opcode::S_BFE_U64:
			return 2u;
		default: return std::max(inst.data_dwords, 1u);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
