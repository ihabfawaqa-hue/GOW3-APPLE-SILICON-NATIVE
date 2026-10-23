// SPDX-License-Identifier: GPL-2.0-or-later
// Port file (not upstream): arm64 replacement for ir/passes/flatten_extended_userdata_pass.cpp.
//
// The upstream pass is x86-64 only: it includes xbyak and Zydis at the top of the file and
// JIT-compiles an SRT walker that chases user-data pointers into the flat buffer. This port
// excludes that file and provides the same entry point without the JIT.
//
// What it does: every ReadConst whose base is a user-data pointer pair and whose index is an
// immediate gets its own slot in flattened_ud_buf. The host fills those slots from guest memory
// before each draw (see ps4_gcn_translate.cpp). Slot 0..15 hold the user data itself, as in
// upstream. Reads with a dynamic index, or with a base that is not a user-data pair, get no
// slot, and the program is marked flat_user_data_unresolved so the caller can reject it.

#include <map>
#include <utility>

#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/program.h"

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* /*ptr*/, size_t /*size*/) {
    return nullptr;
}

namespace Optimization {

void FlattenExtendedUserdataPass(IR::Program& program) {
    Info& info = program.info;
    constexpr u32 kFirstSlot = NUM_USER_DATA_REGS;

    std::map<std::pair<u32, u32>, u16> slot_of; // (ud_lo, dword_offset) -> slot
    u32 next_slot = kFirstSlot;
    bool unresolved = false;

    for (IR::Block* const block : program.post_order_blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst) {
                continue;
            }
            IR::Inst* base = inst.Arg(0).TryInst();
            const IR::Value index = inst.Arg(1);
            if (!base || base->GetOpcode() != IR::Opcode::CompositeConstructU32x2 ||
                !index.IsImmediate()) {
                unresolved = true;
                continue;
            }
            IR::Inst* ptr_lo = base->Arg(0).TryInst();
            if (!ptr_lo || ptr_lo->GetOpcode() != IR::Opcode::GetUserData) {
                unresolved = true;
                continue;
            }

            const std::pair<u32, u32> key{static_cast<u32>(ptr_lo->Arg(0).ScalarReg()),
                                          index.U32()};
            auto it = slot_of.find(key);
            if (it == slot_of.end()) {
                it = slot_of.emplace(key, static_cast<u16>(next_slot++)).first;
                info.flat_user_data_reads.push_back({key.first, key.second, it->second});
            }
            inst.SetFlags(it->second);
        }
    }

    info.srt_info.flattened_bufsize_dw = next_slot;
    info.flat_user_data_unresolved = unresolved;
    info.RefreshFlatBuf();
}

} // namespace Optimization
} // namespace Shader
