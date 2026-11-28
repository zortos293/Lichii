// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/shader_info.h"
#include "shader_recompiler/stage.h"

namespace Shader::IR {

struct Program {
    AbstractSyntaxList syntax_list;
    BlockList blocks;
    BlockList post_order_blocks;
    Info info;
    Stage stage{};
    std::array<u32, 3> workgroup_size{};
    OutputTopology output_topology{};
    u32 output_vertices{};
    u32 invocations{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    /// The program's own shared memory while an HMMA path has appended an area after it: each
    /// emission gives that area back and reserves again, so emitting twice (Nx2Perf::Verify)
    /// does not append a second one
    std::optional<u32> hmma_program_shared_bytes;
    /// What the HMMA paths need to know of the program's TensorMulAdds, found in one pass for each
    /// HMMA plan (PrepareHmma, EmitSPIRV)
    struct HmmaSummary {
        size_t count;      ///< Its TensorMulAdds
        bool any_k16;      ///< One of them is HMMA.16816 (depth 16)
        /// All run where the whole warp does (HmmaInConvergedFlow), looked at only for the staged
        /// exchange, which needs it
        std::optional<bool> converged;
        /// One of them comes after an if or a loop (closed), where lanes meet again only with
        /// maximal reconvergence. Known with `converged`.
        bool after_branches{};
    };
    mutable std::optional<HmmaSummary> hmma_summary;
    /// PrepareHmma set the HMMA requirements in info: emitting checks them instead
    bool hmma_prepared{};
    bool is_geometry_passthrough{};
    MeshInfo mesh{};
    /// Sets of storage buffer descriptor indices, and the global accesses routed among one of them
    /// (an index into the sets) instead of among every bound storage buffer
    std::vector<std::vector<u32>> global_memory_routes;
    std::unordered_map<const Inst*, u32> global_memory_route;
};

[[nodiscard]] std::string DumpProgram(const Program& program);

} // namespace Shader::IR
