// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstring>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include "common/alignment.h"
#include "common/common_funcs.h"
#include "common/game_presets.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/lz4_compression.h"
#include "common/zbic_compression.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/arm/dynarmic/window_trace.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/loader/nso.h"
#include "core/memory.h"

#ifdef HAS_NCE
#include "core/arm/nce/patcher.h"
#endif

namespace Loader {
namespace {
struct MODHeader {
    u32_le magic;
    u32_le dynamic_offset;
    u32_le bss_start_offset;
    u32_le bss_end_offset;
    u32_le eh_frame_hdr_start_offset;
    u32_le eh_frame_hdr_end_offset;
    u32_le module_offset; // Offset to runtime-generated module object. typically equal to .bss base
};
static_assert(sizeof(MODHeader) == 0x1c, "MODHeader has incorrect size.");

constexpr u32 PageAlignSize(u32 size) {
    return static_cast<u32>((size + Core::Memory::LICHII_PAGEMASK) & ~Core::Memory::LICHII_PAGEMASK);
}

struct CodeWord {
    u32 image_offset;
    u32 original;
    u32 replacement;
};

struct BuiltInCodeFix {
    std::string_view build_id;
    std::span<const CodeWord> words;
    std::string_view description;
};

constexpr std::array SwitchSportsResortLanWords{
    CodeWord{0x3337db0, 0x390043f4, 0x390043ff}, CodeWord{0x445ceb0, 0x00000003, 0x00000002},
    CodeWord{0x445cd6c, 0x00000002, 0x00000003}, CodeWord{0x9e27cc, 0x52800049, 0x52800069},
    CodeWord{0x9e41d4, 0x52800056, 0x52800076}, CodeWord{0xd5a30, 0x2a1f03e0, 0x52800020},
    CodeWord{0x31bf8dc, 0x7940142c, 0x5280010c},
    CodeWord{0x8d3cd4, 0x54fff9e1, 0x528000c9}, CodeWord{0x8d3cd8, 0x528000c9, 0x528001ca},
    CodeWord{0x8d3cdc, 0x390033f4, 0x1a8a0129}, CodeWord{0x8d3ce0, 0xb9000be9, 0x290153e9},
};

constexpr std::array SwitchSportsResortJoinListWords{
    CodeWord{0x31c3fc8, 0xa90157f6, 0xf9000bf3}, CodeWord{0x31c3fcc, 0xa9024ff4, 0x9001ac48},
    CodeWord{0x31c3fd0, 0x910003fd, 0xf9420908}, CodeWord{0x31c3fd4, 0x9001ac48, 0xf9403113},
    CodeWord{0x31c3fd8, 0x91104108, 0xf000fc28}, CodeWord{0x31c3fdc, 0xf9400108, 0xf9406908},
    CodeWord{0x31c3fe0, 0xf9403113, 0xf9400100}, CodeWord{0x31c3fe4, 0x9761c143, 0x910083e1},
    CodeWord{0x31c3fe8, 0x7100041f, 0x2a1f03e2}, CodeWord{0x31c3fec, 0x5400028b, 0x97f4ff15},
    CodeWord{0x31c3ff0, 0x2a0003f4, 0x360002a0}, CodeWord{0x31c3ff4, 0x2a1f03f5, 0xaa1303e0},
    CodeWord{0x31c3ff8, 0x2a1f03f6, 0x9406db4e}, CodeWord{0x31c4000, 0x2a1503e1, 0x2a1f03e1},
    CodeWord{0x31c4004, 0x9406d333, 0x9102a262}, CodeWord{0x31c4008, 0x360000a0, 0x910083e3},
    CodeWord{0x31c400c, 0xaa1303e0, 0xaa1f03e4}, CodeWord{0x31c4010, 0x2a1503e1, 0x2a1f03e5},
    CodeWord{0x31c4014, 0x9406d33f, 0x9406d28b}, CodeWord{0x31c4018, 0x52800036, 0xaa1303e0},
    CodeWord{0x31c401c, 0x110006b5, 0x9406d385}, CodeWord{0x31c4020, 0x6b15029f, 0xb9421a68},
    CodeWord{0x31c4024, 0x54fffec1, 0x34000108}, CodeWord{0x31c4028, 0x120002c0, 0xaa1303e0},
    CodeWord{0x31c402c, 0xa9424ff4, 0x9406d415}, CodeWord{0x31c4030, 0xa94157f6, 0xaa1303e0},
    CodeWord{0x31c4034, 0xa8c37bfd, 0x9406d503}, CodeWord{0x31c4038, 0xd65f0fff, 0xd2800029},
    CodeWord{0x31c403c, 0x2a1f03f6, 0xf2c02029}, CodeWord{0x31c4040, 0x17fffffa, 0xf9010269},
    CodeWord{0x31c4044, 0x00000000, 0xf9400bf3}, CodeWord{0x31c4048, 0x00000000, 0xa8c37bfd},
    CodeWord{0x31c404c, 0x00000000, 0xd65f0fff}, CodeWord{0x3376c20, 0xb9423008, 0xb9406008},
    CodeWord{0x3376c24, 0x7100051f, 0x7100091f}, CodeWord{0x3376c74, 0xd0006fa1, 0x2a1f03e0},
    CodeWord{0x3376c78, 0x910b8c21, 0x1400002d}, CodeWord{0x3378eec, 0x36000309, 0xd503201f},
    CodeWord{0x3378ef4, 0xb40002c9, 0xd503201f},
};

constexpr std::array SwitchSportsResortLanJoinWords{
    CodeWord{0x31de478, 0x540004c1, 0x17fd61ee}, CodeWord{0x38f26b4, 0xb50017b5, 0x17e11188},
    CodeWord{0x31deb00, 0x36000100, 0xd503201f}, CodeWord{0x31deb0c, 0x528000a1, 0x528000e1},
    CodeWord{0x3136c30, 0xd503237f, 0x2a0903f5}, CodeWord{0x3136c34, 0xa9bc7bfd, 0x2a0203f6},
    CodeWord{0x3136c38, 0xf9000bf7, 0xaa0803f7}, CodeWord{0x3136c3c, 0xa90257f6, 0x941f7f01},
    CodeWord{0x3136c40, 0xa9034ff4, 0x71000c1f}, CodeWord{0x3136c44, 0x910003fd, 0x540000e0},
    CodeWord{0x3136c48, 0xaa0103f5, 0x2a1503e9}, CodeWord{0x3136c4c, 0xaa0003f3, 0x2a1603e2},
    CodeWord{0x3136c50, 0x9760d110, 0xaa1703e8}, CodeWord{0x3136c54, 0xf94022a8, 0x7100053f},
    CodeWord{0x3136c58, 0x2a0003f4, 0x5453c5c1}, CodeWord{0x3136c5c, 0x91523917, 0x14029e08},
    CodeWord{0x3136c60, 0xb90da6e0, 0x2a1f03f8}, CodeWord{0x3136c64, 0xf946cee0, 0x710006bf},
    CodeWord{0x3136c68, 0xb4000160, 0x54000281}, CodeWord{0x3136c6c, 0x97688751, 0xf94032f7},
    CodeWord{0x3136c70, 0xaa0003f6, 0xb4000257}, CodeWord{0x3136c74, 0x97688753, 0x52857208},
    CodeWord{0x3136c78, 0xb94da6e8, 0x72a00188}, CodeWord{0x3136c7c, 0x7100151f, 0x8b0802e0},
    CodeWord{0x3136c80, 0x540001a8, 0x943d0ef0}, CodeWord{0x3136c84, 0xf0009909, 0xb942a2e8},
    CodeWord{0x3136c88, 0x911ab129, 0x6b0802df}, CodeWord{0x3136c8c, 0xb8687921, 0x540000e2},
    CodeWord{0x3136c90, 0x1400000a, 0xf94156e8}, CodeWord{0x3136c94, 0x9768874b, 0xf8765900},
    CodeWord{0x3136c98, 0xb94da6e8, 0xb4000080}, CodeWord{0x3136c9c, 0x7100151f, 0x91002000},
    CodeWord{0x3136ca0, 0x540004e8, 0x943d0fac}, CodeWord{0x3136ca4, 0xf0009909, 0x5ac00818},
    CodeWord{0x3136ca8, 0x911ab129, 0x52857208}, CodeWord{0x3136cac, 0xb8687921, 0x72a00188},
    CodeWord{0x3136cb0, 0x14000024, 0x8b0802e0}, CodeWord{0x3136cb4, 0x2a1f03e1, 0x943d0ee7},
    CodeWord{0x3136cb8, 0xb94da2e3, 0xf9409288}, CodeWord{0x3136cbc, 0xaa1603e2, 0xb9000918},
    CodeWord{0x3136cc0, 0x976591a8, 0xaa1303e0}, CodeWord{0x3136cc4, 0xf94022a8, 0x52800041},
    CodeWord{0x3136cc8, 0xaa1303e0, 0x2a1f03e2}, CodeWord{0x3136ccc, 0x91523915, 0x974aeb21},
    CodeWord{0x3136cd0, 0x9760d0f0, 0x14029e1a}, CodeWord{0x3136cd4, 0x71000a9f, 0xb4000055},
    CodeWord{0x3136cd8, 0x54000141, 0x141eef34}, CodeWord{0x3136cdc, 0xb90e96a0, 0xb9400a88},
    CodeWord{0x3136ce0, 0xf94746a0, 0x71000d1f}, CodeWord{0x3136ce4, 0xb4000340, 0x540005a1},
    CodeWord{0x3136ce8, 0x97688732, 0xb001b0a8}, CodeWord{0x3136cec, 0xaa0003f3, 0xf9420108},
    CodeWord{0x3136cf0, 0x97688734, 0xb4000548}, CodeWord{0x3136cf4, 0xb94e96a1, 0xb9400109},
    CodeWord{0x3136cf8, 0xb94e92a3, 0xf9400908}, CodeWord{0x3136cfc, 0x14000009, 0xf8697908},
    CodeWord{0x3136d00, 0xb90e1ea0, 0xb40004c8}, CodeWord{0x3136d04, 0xf9470aa0, 0xf9400508},
    CodeWord{0x3136d08, 0xb40002a0, 0xb4000488}, CodeWord{0x3136d0c, 0x97688729, 0xf940a108},
    CodeWord{0x3136d10, 0xaa0003f3, 0xb4000448}, CodeWord{0x3136d14, 0x9768872b, 0xb9400916},
    CodeWord{0x3136d18, 0xb94e1ea1, 0xb900091f}, CodeWord{0x3136d1c, 0xb94e1aa3, 0x340003f6},
    CodeWord{0x3136d20, 0xaa1303e2, 0xf9400268}, CodeWord{0x3136d24, 0xa9434ff4, 0xaa1303e0},
    CodeWord{0x3136d28, 0xa94257f6, 0xf9401108}, CodeWord{0x3136d2c, 0xf9400bf7, 0xd63f0100},
    CodeWord{0x3136d30, 0xa8c47bfd, 0xaa0003e1}, CodeWord{0x3136d34, 0xd50323ff, 0xaa1403e0},
    CodeWord{0x3136d38, 0x17658e3e, 0x941ee7b2}, CodeWord{0x3136d3c, 0x2a1f03e1, 0x9100a3e8},
    CodeWord{0x3136d40, 0xb94da2e2, 0x941ef6d0}, CodeWord{0x3136d44, 0x9427c86b, 0xb9402be8},
    CodeWord{0x3136d48, 0x17ffffdf, 0x35000288}, CodeWord{0x3136d4c, 0x9768871d, 0xd00179a8},
    CodeWord{0x3136d50, 0xb94e96a1, 0xf940c508}, CodeWord{0x3136d54, 0xb94e92a2, 0xf9403d08},
    CodeWord{0x3136d58, 0x14000004, 0xb4000208}, CodeWord{0x3136d5c, 0x97688719, 0xf9400909},
    CodeWord{0x3136d60, 0xb94e1ea1, 0x7940110a}, CodeWord{0x3136d64, 0xb94e1aa2, 0x340001aa},
    CodeWord{0x3136d68, 0xa9434ff4, 0xf8408521}, CodeWord{0x3136d6c, 0xa94257f6, 0x5100054a},
    CodeWord{0x3136d70, 0xf9400bf7, 0xb4ffffa1}, CodeWord{0x3136d74, 0xa8c47bfd, 0xb942c02b},
    CodeWord{0x3136d78, 0xd50323ff, 0x6b16017f}, CodeWord{0x3136d7c, 0x1427b991, 0x54ffff41},
    CodeWord{0x3136d80, 0xd503237f, 0xaa1403e0}, CodeWord{0x3136d84, 0xa9be7bfd, 0x941ee823},
    CodeWord{0x3136d88, 0xa9014ff4, 0xb4000080}, CodeWord{0x3136d8c, 0x910003fd, 0xf9400008},
    CodeWord{0x3136d90, 0xf9402028, 0xf9400d08}, CodeWord{0x3136d94, 0x91523914, 0xd63f0100},
    CodeWord{0x3136d98, 0x9760d0be, 0x141eee48}, CodeWord{0x3136d9c, 0xb90f5e80, 0x141eee94},
};

constexpr std::array SwitchSportsResortInviteIdGuardWords{
    CodeWord{0x31df514, 0x910f63e8, 0x17fd5e23}, CodeWord{0x31c4648, 0xaa1f03f7, 0x14000019},
    CodeWord{0x31d60cc, 0xaa0003e8, 0xb4fff9a0}, CodeWord{0x31d60d0, 0xd10263a0, 0x91002001},
    CodeWord{0x31d60d4, 0x91002101, 0xd10263a0}, CodeWord{0x31de5dc, 0xaa0003e8, 0xb4fffa20},
    CodeWord{0x31de5e0, 0xd10263a0, 0x91002001}, CodeWord{0x31de5e4, 0x91002101, 0xd10263a0},
    CodeWord{0x3136da0, 0xf947aa80, 0xb4543cc0}, CodeWord{0x3136da4, 0xb4000160, 0x910f63e8},
    CodeWord{0x3136da8, 0x97688702, 0x1402a1dc},
};

constexpr std::array SwitchSportsResortHostOnWords{
    CodeWord{0x31dd8d0, 0x52800048, 0x17fd672c}, CodeWord{0x3137580, 0xf9400808, 0x52800048},
    CodeWord{0x3137584, 0xb9400108, 0xb9001008}, CodeWord{0x3137588, 0x71003d1f, 0xa9bd7bfd},
    CodeWord{0x313758c, 0x5400024c, 0xf9000be0}, CodeWord{0x3137590, 0xf9403429, 0x910003fd},
    CodeWord{0x3137594, 0xb980702a, 0xd2800028}, CodeWord{0x3137598, 0xf9400408, 0xf2fdabc8},
    CodeWord{0x313759c, 0x8b0a0129, 0xd2ca6a89}, CodeWord{0x31375a0, 0x9120c108, 0xf2e909e9},
    CodeWord{0x31375a4, 0x381ff13f, 0xa90227e8}, CodeWord{0x31375a8, 0xf9400809, 0x910063e0},
    CodeWord{0x31375ac, 0xb9400129, 0x910083e1}, CodeWord{0x31375b0, 0x8b090d0a, 0x943d1510},
    CodeWord{0x31375b4, 0x7100413f, 0xf9400be0}, CodeWord{0x31375b8, 0xf9403429, 0xa8c37bfd},
    CodeWord{0x31375bc, 0x9a883148, 0xd65f03c0},
};

constexpr std::array SwitchSportsResortHostOffWords{
    CodeWord{0x31def70, 0xb000fb68, 0x17fd6194}, CodeWord{0x31375c0, 0xf9000109, 0xa9bd7bfd},
    CodeWord{0x31375c4, 0xf9400808, 0x910003fd}, CodeWord{0x31375c8, 0xb9400109, 0xd2fdabc8},
    CodeWord{0x31375cc, 0x11000529, 0xd2ca6a89}, CodeWord{0x31375d0, 0xb9000109, 0xf2e909e9},
    CodeWord{0x31375d4, 0xd65f03c0, 0xa90227e8}, CodeWord{0x31375d8, 0x00000000, 0x910063e0},
    CodeWord{0x31375dc, 0x00000000, 0x910083e1}, CodeWord{0x31375e0, 0xb000f3a8, 0x943d1504},
    CodeWord{0x31375e4, 0x9122c108, 0xa8c37bfd}, CodeWord{0x31375e8, 0xf9000028, 0x900100a8},
    CodeWord{0x31375ec, 0xa940a408, 0x14029e62},
};

constexpr std::array SwitchSportsResortHuddleLeaveWords{
    CodeWord{0x31e29b8, 0x350000a8, 0xd503201f}, CodeWord{0x31e2c94, 0x35000208, 0xd503201f},
    CodeWord{0x31e3c6c, 0x370000a0, 0x14000005}, CodeWord{0x31e3ce0, 0x52800068, 0x52800048},
    CodeWord{0x31e2cc8, 0x52800121, 0x52800181},
};

constexpr std::array SwitchSportsResortHuddleNoHangWords{
    CodeWord{0x31371d0, 0xb988b80a, 0xaa1303e0}, CodeWord{0x31371d4, 0x396bc008, 0x1402a578},
    CodeWord{0x31371e0, 0x912b4009, 0x52800028}, CodeWord{0x31371e4, 0x8b0a156a, 0xb901a268},
    CodeWord{0x31371e8, 0x7100011f, 0x52800048}, CodeWord{0x31371ec, 0x9a8a1128, 0xb901da68},
    CodeWord{0x31371f0, 0x39406100, 0x14024d86}, CodeWord{0x31ca730, 0xb40006c0, 0xb4b65580},
    CodeWord{0x31e0850, 0xb4000160, 0xb4ab4c00},
};

constexpr std::array SwitchSportsResortLobbyMaxWords{
    CodeWord{0x31c69d4, 0x34000040, 0x52800100},
};

constexpr std::array SwitchSportsResortBusPumpWords{
    CodeWord{0x1dd15c, 0x97fffe89, 0x94fa8b8d}, CodeWord{0x407ff80, 0x00000000, 0xd40f4381},
    CodeWord{0x407ff84, 0x00000000, 0xd65f03c0}, CodeWord{0x407ff88, 0x00000000, 0x2a1f03e0},
    CodeWord{0x407ff8c, 0x00000000, 0xd65f03c0}, CodeWord{0x407ff90, 0x00000000, 0xa9be7bfd},
    CodeWord{0x407ff94, 0x00000000, 0x910003fd}, CodeWord{0x407ff98, 0x00000000, 0xa90153f3},
    CodeWord{0x407ff9c, 0x00000000, 0x970572f9}, CodeWord{0x407ffa0, 0x00000000, 0x90013668},
    CodeWord{0x407ffa4, 0x00000000, 0xf9420908}, CodeWord{0x407ffa8, 0x00000000, 0xb4000268},
    CodeWord{0x407ffac, 0x00000000, 0xf9403513}, CodeWord{0x407ffb0, 0x00000000, 0xb4000233},
    CodeWord{0x407ffb4, 0x00000000, 0x52800214}, CodeWord{0x407ffb8, 0x00000000, 0x52800040},
    CodeWord{0x407ffbc, 0x00000000, 0xaa1f03e1}, CodeWord{0x407ffc0, 0x00000000, 0xaa1f03e2},
    CodeWord{0x407ffc4, 0x00000000, 0xf0013843}, CodeWord{0x407ffc8, 0x00000000, 0x52880004},
    CodeWord{0x407ffcc, 0x00000000, 0xd40f4381}, CodeWord{0x407ffd0, 0x00000000, 0x7100041f},
    CodeWord{0x407ffd4, 0x00000000, 0x5400010b}, CodeWord{0x407ffd8, 0x00000000, 0x2a0003e2},
    CodeWord{0x407ffdc, 0x00000000, 0xaa1303e0}, CodeWord{0x407ffe0, 0x00000000, 0xf0013841},
    CodeWord{0x407ffe4, 0x00000000, 0x97ffffe9}, CodeWord{0x407ffe8, 0x00000000, 0x35000060},
    CodeWord{0x407ffec, 0x00000000, 0x71000694}, CodeWord{0x407fff0, 0x00000000, 0x54fffe41},
    CodeWord{0x407fff4, 0x00000000, 0xa94153f3}, CodeWord{0x407fff8, 0x00000000, 0xa8c27bfd},
    CodeWord{0x407fffc, 0x00000000, 0xd65f03c0},
};

constexpr std::array SwitchSportsResortIdentityWords{
    CodeWord{0x31331f0, 0xd503237f, 0xa9bb7bfd}, CodeWord{0x31331f4, 0xa9be7bfd, 0x910003fd},
    CodeWord{0x31331f8, 0xf9000bf3, 0xa90153f3}, CodeWord{0x31331fc, 0x910003fd, 0xaa0003f3},
    CodeWord{0x3133200, 0xaa0003f3, 0x2a0103f4}, CodeWord{0x3133204, 0x97e1293b, 0xf90013ff},
    CodeWord{0x3133208, 0xb000f3c8, 0xf90017ff}, CodeWord{0x313320c, 0x91108108, 0xb90033ff},
    CodeWord{0x3133210, 0x3900427f, 0x910083e0}, CodeWord{0x3133214, 0x91005260, 0x910093e1},
    CodeWord{0x3133218, 0xf9000268, 0x9100a3e2}, CodeWord{0x313321c, 0xf9400bf3, 0x9100b3e3},
    CodeWord{0x3133220, 0xa8c27bfd, 0x9100c3e4}, CodeWord{0x3133224, 0xd50323ff, 0x943d2673},
    CodeWord{0x3133228, 0x1402eed2, 0xb94023e8}, CodeWord{0x313322c, 0x00000000, 0x5ac00908},
    CodeWord{0x3133230, 0xd503237f, 0x4a147d14}, CodeWord{0x3133234, 0xa9be7bfd, 0xb4000153},
    CodeWord{0x3133238, 0xf9000bf3, 0x9100e3e0}, CodeWord{0x313323c, 0x910003fd, 0x52800201},
    CodeWord{0x3133240, 0xb000f3c8, 0xb00083c2}, CodeWord{0x3133244, 0x91108108, 0x913a1c42},
    CodeWord{0x3133248, 0xaa0003f3, 0x2a1403e3}, CodeWord{0x313324c, 0xf8014408, 0x943d1f65},
    CodeWord{0x3133250, 0x9402eecc, 0xaa1303e0}, CodeWord{0x3133254, 0xaa1303e0, 0x9100e3e1},
    CodeWord{0x3133258, 0xf9400bf3, 0x940269e2}, CodeWord{0x313325c, 0xa8c27bfd, 0x2a1403e0},
    CodeWord{0x3133260, 0xd50323ff, 0xa94153f3}, CodeWord{0x3133264, 0x17e12927, 0xa8c57bfd},
    CodeWord{0x3133268, 0x00000000, 0xd65f03c0}, CodeWord{0x3133270, 0xd503237f, 0xa9bf7bfd},
    CodeWord{0x3133274, 0xa9be7bfd, 0x910003fd}, CodeWord{0x3133278, 0xf9000bf3, 0x39410268},
    CodeWord{0x313327c, 0x910003fd, 0x35000088}, CodeWord{0x3133280, 0xb000f3c8, 0x91010260},
    CodeWord{0x3133284, 0x91108108, 0x52800001}, CodeWord{0x3133288, 0xaa0003f3, 0x97ffffda},
    CodeWord{0x313328c, 0xf8014408, 0x91010260}, CodeWord{0x3133290, 0x9402eebc, 0xa8c17bfd},
    CodeWord{0x3133294, 0xaa1303e0, 0xd65f03c0}, CodeWord{0x31332a0, 0xf9400bf3, 0xa9bf7bfd},
    CodeWord{0x31332a4, 0x52800401, 0x910003fd}, CodeWord{0x31332a8, 0xa8c27bfd, 0x9765a12a},
    CodeWord{0x31332ac, 0xd50323ff, 0x940000c1}, CodeWord{0x31332b0, 0x173c33dc, 0xa8c17bfd},
    CodeWord{0x31332b4, 0x00000000, 0xd65f03c0}, CodeWord{0x31332c0, 0xd503237f, 0xa9b87bfd},
    CodeWord{0x31332c4, 0xa9be7bfd, 0x910003fd}, CodeWord{0x31332c8, 0xa9014ff4, 0xa90153f3},
    CodeWord{0x31332cc, 0x910003fd, 0xa9025bf5}, CodeWord{0x31332d0, 0xaa0103f3, 0xa90363f7},
    CodeWord{0x31332d4, 0xaa0003f4, 0xa9046bf9}, CodeWord{0x31332d8, 0x9764052a, 0x941f8d5a},
    CodeWord{0x31332dc, 0x360000a0, 0x71000c1f}, CodeWord{0x31332e0, 0xa9414ff4, 0x54000e81},
    CodeWord{0x31332e4, 0x2a1f03e0, 0x9001b0c8}, CodeWord{0x31332e8, 0xa8c27bfd, 0xf9420113},
    CodeWord{0x31332ec, 0xd65f0fff, 0xb4000e33}, CodeWord{0x31332f0, 0xaa1403e0, 0xb9400269},
    CodeWord{0x31332f4, 0xaa1303e1, 0xb9401a6a}, CodeWord{0x31332f8, 0xa9414ff4, 0x6b0a013f},
    CodeWord{0x31332fc, 0xa8c27bfd, 0x54000da2}, CodeWord{0x3133300, 0xd50323ff, 0xf9401268},
    CodeWord{0x3133304, 0x14000003, 0xf8695914}, CodeWord{0x3133308, 0x00000000, 0xb4000d54},
    CodeWord{0x313330c, 0x00000000, 0x91010260}, CodeWord{0x3133310, 0xd503237f, 0x52800001},
    CodeWord{0x3133314, 0xa9be7bfd, 0x97ffffb7}, CodeWord{0x3133318, 0xa9014ff4, 0x97640476},
    CodeWord{0x313331c, 0x910003fd, 0x7100081f}, CodeWord{0x3133320, 0xaa0103f3, 0x52800048},
    CodeWord{0x3133324, 0xaa0003f4, 0x1a88b016}, CodeWord{0x3133328, 0x975b281a, 0x52800015},
    CodeWord{0x313332c, 0xf9401408, 0x6b1602bf}, CodeWord{0x3133330, 0xf9400908, 0x540007aa},
    CodeWord{0x3133334, 0xf9400108, 0xa9057fff}, CodeWord{0x3133338, 0x3947c108, 0xf90033ff},
    CodeWord{0x313333c, 0x360000a8, 0x910143e0}, CodeWord{0x3133340, 0x2a1f03e0, 0x2a1503e1},
    CodeWord{0x3133344, 0xa9414ff4, 0x97ffffab}, CodeWord{0x3133348, 0xa8c27bfd, 0x2a0003f9},
    CodeWord{0x313334c, 0xd65f0fff, 0x3901a3f5}, CodeWord{0x3133350, 0xaa1403e0, 0xaa1403e0},
    CodeWord{0x3133354, 0x97e128f7, 0x9101a3e1}, CodeWord{0x3133358, 0x36000060, 0x94095e76},
    CodeWord{0x313335c, 0x52800020, 0xaa0003f7}, CodeWord{0x3133360, 0x17fffff9, 0xb4000120},
    CodeWord{0x3133364, 0xaa1403e0, 0xf94006e0}, CodeWord{0x3133368, 0x9400000a, 0x910147e1},
    CodeWord{0x313336c, 0x39404288, 0xd2800122}, CodeWord{0x3133370, 0x7100051f, 0x943d1b40},
    CodeWord{0x3133374, 0x54fffe61, 0x34000540}, CodeWord{0x3133378, 0x91005280, 0xaa1703e0},
    CodeWord{0x313337c, 0xaa1303e1, 0x94095be1}, CodeWord{0x3133380, 0xa9414ff4, 0x1400000d},
    CodeWord{0x3133384, 0xa8c27bfd, 0xb9801a88}, CodeWord{0x3133388, 0xd50323ff, 0x340004a8},
    CodeWord{0x313338c, 0x1402ee81, 0xf9401297}, CodeWord{0x3133390, 0xd503237f, 0x52806709},
    CodeWord{0x3133394, 0xa9be7bfd, 0x9b295d1a}, CodeWord{0x3133398, 0xf9000bf3, 0xf94006e9},
    CodeWord{0x313339c, 0x910003fd, 0x39400129}, CodeWord{0x31333a0, 0xaa0003f3, 0x340000a9},
    CodeWord{0x31333a4, 0x91005000, 0x910ce2f7}, CodeWord{0x31333a8, 0x974280a6, 0xeb1a02ff},
    CodeWord{0x31333ac, 0x39404269, 0x54ffff61}, CodeWord{0x31333b0, 0x12000008, 0x1400001b},
    CodeWord{0x31333b4, 0x6b08013f, 0x975b27f7}, CodeWord{0x31333b8, 0x54000081, 0xf9401400},
    CodeWord{0x31333bc, 0xf9400bf3, 0x9101a3e1}, CodeWord{0x31333c0, 0xa8c27bfd, 0x94023160},
    CodeWord{0x31333c4, 0xd65f0fff, 0x2a0003e1}, CodeWord{0x31333c8, 0x39004268, 0xf00100a8},
    CodeWord{0x31333cc, 0x34000108, 0xf9406108}, CodeWord{0x31333d0, 0x97650d04, 0xf9400100},
    CodeWord{0x31333d4, 0x2a0003e1, 0x97f74de3}, CodeWord{0x31333d8, 0x91005260, 0x39400008},
    CodeWord{0x31333dc, 0xf9400bf3, 0x7100051f}, CodeWord{0x31333e0, 0xa8c27bfd, 0x540001e1},
    CodeWord{0x31333e4, 0xd50323ff, 0xaa0003e6}, CodeWord{0x31333e8, 0x1402ee9e, 0xaa1703e0},
    CodeWord{0x31333ec, 0x91005260, 0xaa1703e1}, CodeWord{0x31333f0, 0xf9400bf3, 0x91004282},
    CodeWord{0x31333f4, 0xa8c27bfd, 0x910143e3}, CodeWord{0x31333f8, 0xd50323ff, 0x2a1503e4},
    CodeWord{0x31333fc, 0x1402ee95, 0x2a1903e5}, CodeWord{0x3133400, 0xd503237f, 0x94028298},
    CodeWord{0x3133404, 0xa9bd7bfd, 0xf9401ee0}, CodeWord{0x3133408, 0xf9000bf5, 0xb98042e1},
    CodeWord{0x313340c, 0xa9024ff4, 0xb00083c2}, CodeWord{0x3133410, 0x910003fd, 0x913a1c42},
    CodeWord{0x3133414, 0xaa0003f3, 0x2a1903e3}, CodeWord{0x3133418, 0x7100085f, 0x943d1ef2},
    CodeWord{0x313341c, 0x540004a0, 0x110006b5}, CodeWord{0x3133420, 0x7100045f, 0x17ffffc3},
    CodeWord{0x3133424, 0x54000100, 0x9402f287}, CodeWord{0x3133428, 0x350006c2, 0xf9401a89},
    CodeWord{0x313342c, 0xa9424ff4, 0xb9802a8a}, CodeWord{0x3133430, 0xf9400bf5, 0xf940328b},
    CodeWord{0x3133434, 0xaa0103e0, 0xf9403e8c}, CodeWord{0x3133438, 0xa8c37bfd, 0x5280000d},
    CodeWord{0x313343c, 0xd50323ff, 0xb40002ea}, CodeWord{0x3133440, 0x17685704, 0xf940052e},
    CodeWord{0x3133444, 0xaa0103e0, 0xb400024e}, CodeWord{0x3133448, 0x97685702, 0xf94005cf},
    CodeWord{0x313344c, 0xaa0003f5, 0x394001ef}, CodeWord{0x3133450, 0xf8430ea8, 0x340001ef},
    CodeWord{0x3133454, 0xaa0003f4, 0xf940092f}, CodeWord{0x3133458, 0xaa1503e0, 0xb40001af},
    CodeWord{0x313345c, 0xf9400d08, 0xb94001d0}, CodeWord{0x3133460, 0xd63f0100, 0xb94071f1},
    CodeWord{0x3133464, 0xb000f3c8, 0xb94079cf}, CodeWord{0x3133468, 0x91132108, 0x33040e11},
    CodeWord{0x313346c, 0xf900269f, 0xaa1181ef}, CodeWord{0x3133470, 0xf90002a8, 0x8b2d5170},
    CodeWord{0x3133474, 0xd503201f, 0xf900020f}, CodeWord{0x3133478, 0x100004c8, 0x8b2d5190},
    CodeWord{0x313347c, 0xaa1403f5, 0xf900020f}, CodeWord{0x3133480, 0xa903a293, 0x110005ad},
    CodeWord{0x3133484, 0xf8470ea8, 0x710041bf}, CodeWord{0x3133488, 0xaa1503e0, 0x5400008a},
    CodeWord{0x313348c, 0xf9400d08, 0x91006129}, CodeWord{0x3133490, 0xd63f0100, 0xd100054a},
    CodeWord{0x3133494, 0xb000f3c8, 0x17ffffea}, CodeWord{0x3133498, 0x91140108, 0xb9003a9f},
    CodeWord{0x313349c, 0xaa1403e0, 0xb900528d}, CodeWord{0x31334a0, 0xf90002a8, 0xb9006a8d},
    CodeWord{0x31334a4, 0xd503201f, 0x3400006d}, CodeWord{0x31334a8, 0x100003c8, 0xaa1403e0},
    CodeWord{0x31334ac, 0x1400000f, 0x94095ee1}, CodeWord{0x31334b0, 0xaa0103e0, 0xa9446bf9},
    CodeWord{0x31334b4, 0x976856e7, 0xa94363f7}, CodeWord{0x31334b8, 0xaa0003f5, 0xa9425bf5},
    CodeWord{0x31334bc, 0xf8470ea8, 0xa94153f3}, CodeWord{0x31334c0, 0xaa0003f4, 0xa8c87bfd},
    CodeWord{0x31334c4, 0xaa1503e0, 0xd65f03c0}, CodeWord{0x3387db4, 0x91010260, 0x97f6ad2f},
    CodeWord{0x333cafc, 0x975d7b15, 0x97f7d9e9}, CodeWord{0x31e050c, 0x540001a1, 0x540001ab},
    CodeWord{0x31334d0, 0xb000f3c8, 0xa9bd7bfd}, CodeWord{0x31334d4, 0x91140108, 0x910003fd},
    CodeWord{0x31334d8, 0xaa1403e0, 0xa90157f4}, CodeWord{0x31334dc, 0xf90002a8, 0xf9413e74},
    CodeWord{0x31334e0, 0xd503201f, 0xb40005b4}, CodeWord{0x31334e4, 0x100004e8, 0x941f8cd7},
    CodeWord{0x31334e8, 0xa907a293, 0x71000c1f}, CodeWord{0x31334ec, 0xf900469f, 0x54000541},
    CodeWord{0x31334f0, 0xa9424ff4, 0x9001b0c8}, CodeWord{0x31334f4, 0xf9400bf5, 0xf9420115},
    CodeWord{0x31334f8, 0xa8c37bfd, 0xb40004f5}, CodeWord{0x31334fc, 0xd65f0fff, 0xb94002a9},
    CodeWord{0x3133500, 0xaa1f03e0, 0xb9400aaa}, CodeWord{0x3133504, 0x17fffffb, 0x6b0a013f},
    CodeWord{0x3133508, 0x00000000, 0x54000462}, CodeWord{0x313350c, 0x00000000, 0xf9400aa8},
    CodeWord{0x3133510, 0x3903001f, 0xf8695900}, CodeWord{0x3133514, 0xd65f03c0, 0xb4000400},
    CodeWord{0x3133518, 0x00000000, 0xd503201f}, CodeWord{0x313351c, 0x00000000, 0xd503201f},
    CodeWord{0x3133520, 0xb9409008, 0xb94002a9}, CodeWord{0x3133524, 0x71000d1f, 0xb9401aaa},
    CodeWord{0x3133528, 0x54000240, 0x6b0a013f}, CodeWord{0x313352c, 0xd503237f, 0x54000302},
    CodeWord{0x3133530, 0xa9be7bfd, 0xf94012a8}, CodeWord{0x3133534, 0xf9000bf3, 0xf8695915},
    CodeWord{0x3133538, 0x910003fd, 0xb40002b5}, CodeWord{0x313353c, 0xaa0003f3, 0x3943a288},
    CodeWord{0x3133540, 0xaa0103e0, 0x34000108}, CodeWord{0x3133544, 0x2a0803e1, 0x3900a3ff},
    CodeWord{0x3133548, 0x2a1f03e2, 0xaa1503e0}, CodeWord{0x313354c, 0x974af901, 0x9100a3e1},
    CodeWord{0x3133550, 0xb9409268, 0x94095df8}, CodeWord{0x3133554, 0x71000d1f, 0xb4000060},
    CodeWord{0x3133558, 0x54000060, 0xf84ec288}, CodeWord{0x313355c, 0x52800068, 0xf9018c08},
    CodeWord{0x3133560, 0xb9009268, 0x3943d288}, CodeWord{0x3133564, 0xf9400bf3, 0x34000128},
    CodeWord{0x3133568, 0xa8c27bfd, 0x52800028}, CodeWord{0x313356c, 0xd50323ff, 0x3900a3e8},
    CodeWord{0x3133570, 0xd65f03c0, 0xaa1503e0}, CodeWord{0x3133574, 0x00000000, 0x9100a3e1},
    CodeWord{0x3133578, 0x00000000, 0x94095dee}, CodeWord{0x313357c, 0x00000000, 0xb4000060},
    CodeWord{0x3133580, 0xd503237f, 0xf9407e88}, CodeWord{0x3133584, 0xa9be7bfd, 0xf9018c08},
    CodeWord{0x3133588, 0xa9014ff4, 0x9400000a}, CodeWord{0x313358c, 0x910003fd, 0x3903a29f},
    CodeWord{0x3133590, 0xaa0103f3, 0x3903d29f}, CodeWord{0x3133594, 0xaa0003f4, 0xa94157f4},
    CodeWord{0x3133598, 0x940337aa, 0xa8c37bfd}, CodeWord{0x313359c, 0x91026281, 0x52800021},
    CodeWord{0x31335a0, 0x940304d0, 0xf9412260}, CodeWord{0x31335a4, 0x36000080, 0xd65f03c0},
    CodeWord{0x31c8250, 0xf9412000, 0x97fdaca0}, CodeWord{0x31335b0, 0xd65f0fff, 0xa9be7bfd},
    CodeWord{0x31335b4, 0xaa1303e0, 0x910003fd}, CodeWord{0x31335b8, 0xa9414ff4, 0xf9000bf3},
    CodeWord{0x31335bc, 0x52800021, 0xaa1f03f3}, CodeWord{0x31335c0, 0x2a1f03e2, 0x9001b0c8},
    CodeWord{0x31335c4, 0xa8c27bfd, 0xf9420108}, CodeWord{0x31335c8, 0xd50323ff, 0xb4000148},
    CodeWord{0x31335cc, 0x174af8e1, 0xb9400109}, CodeWord{0x31335d0, 0xd503237f, 0xb940090a},
    CodeWord{0x31335d4, 0xa9be7bfd, 0x6b0a013f}, CodeWord{0x31335d8, 0xf9000bf3, 0x540000c2},
    CodeWord{0x31335dc, 0x910003fd, 0xf940090a}, CodeWord{0x31335e0, 0xaa0003f3, 0xf8695953},
    CodeWord{0x31335e4, 0x974d2073, 0xb4000073}, CodeWord{0x31335e8, 0xf00100a8, 0x9102c260},
    CodeWord{0x31335ec, 0xb000f3c9, 0x943d1c95}, CodeWord{0x31335f0, 0x91112129, 0x97ffff34},
    CodeWord{0x31335f4, 0xf9423908, 0xb4000073}, CodeWord{0x31335f8, 0xf9000269, 0x9102c260},
    CodeWord{0x31335fc, 0xaa1303e9, 0x943d1c95}, CodeWord{0x3133600, 0xa9027e7f, 0xf9400bf3},
    CodeWord{0x3133604, 0x91004108, 0xa8c27bfd}, CodeWord{0x3133608, 0xa901227f, 0xd65f03c0},
};

constexpr std::array SwitchSportsResortDocBusWords{
    CodeWord{0x3382470, 0xa95d2809, 0x17f6cb9f}, CodeWord{0x3382ad0, 0xa95d2809, 0x17f6ca0a},
    CodeWord{0x33826a0, 0xa95d2809, 0x17f6cb19}, CodeWord{0x3382cb0, 0xa95d2809, 0x17f6c998},
    CodeWord{0x337d390, 0xd503237f, 0x17f6e03e}, CodeWord{0x337d3d0, 0xd503237f, 0x17f6e03a},
    CodeWord{0x3135000, 0xd65f03c0, 0x51434f44}, CodeWord{0x3135004, 0x00000000, 0x00000001},
    CodeWord{0x3135008, 0x00000000, 0xa9bf7bfd}, CodeWord{0x313500c, 0x00000000, 0x910003fd},
    CodeWord{0x3135010, 0x396bc008, 0x52800060}, CodeWord{0x3135014, 0x36000048, 0x10ffff61},
    CodeWord{0x3135018, 0xd65f03c0, 0xd2800102}, CodeWord{0x313501c, 0xd503237f, 0xaa1f03e3},
    CodeWord{0x3135020, 0xa9be7bfd, 0xaa1f03e4}, CodeWord{0x3135024, 0xf9000bf3, 0x943d2bd7},
    CodeWord{0x3135028, 0x910003fd, 0x7100041f}, CodeWord{0x313502c, 0xaa0003f3, 0x1a9f17e0},
    CodeWord{0x3135030, 0x52a00080, 0xa8c17bfd}, CodeWord{0x3135034, 0x940004f3, 0xd65f03c0},
    CodeWord{0x3135038, 0x36000260, 0xa9bd7bfd}, CodeWord{0x313503c, 0xb948ba69, 0x910003fd},
    CodeWord{0x3135040, 0x5280006b, 0xa90153f3}, CodeWord{0x3135044, 0xf000f3ac, 0xf90013f5},
    CodeWord{0x3135048, 0x9114a18c, 0xaa0003f3}, CodeWord{0x313504c, 0xaa1303e8, 0xaa0103f4},
    CodeWord{0x3135050, 0x2a0903ea, 0xd00100e8}, CodeWord{0x3135054, 0x5100054d, 0xf9476100},
    CodeWord{0x3135058, 0x6b0901bf, 0x52800601}, CodeWord{0x313505c, 0x540003e0, 0x977c18c9},
    CodeWord{0x3135060, 0x7100155f, 0xaa0003f5}, CodeWord{0x3135064, 0x1a9fb1ad, 0xb4000115},
    CodeWord{0x3135068, 0x7100055f, 0xaa1f03e1}, CodeWord{0x313506c, 0x1a8db16a, 0x9772f839},
    CodeWord{0x3135070, 0x937b7d4d, 0xb40000b3}, CodeWord{0x3135074, 0xf86d698d, 0xaa1503e0},
    CodeWord{0x3135078, 0x394001ad, 0xaa1303e1}, CodeWord{0x313507c, 0x34fffecd, 0x2a1403e2},
    CodeWord{0x3135080, 0x14000017, 0x977e7b88}, CodeWord{0x3135084, 0x52a00100, 0xaa1503e0},
    CodeWord{0x3135088, 0x940004de, 0xf94013f5}, CodeWord{0x313508c, 0x36000320, 0xa94153f3},
    CodeWord{0x3135090, 0xb948ba69, 0xa8c37bfd}, CodeWord{0x3135094, 0x5280006b, 0xd65f03c0},
    CodeWord{0x3135098, 0xf000f3ac, 0xa9bd7bfd}, CodeWord{0x313509c, 0x9114a18c, 0x910003fd},
    CodeWord{0x31350a0, 0xaa1303e8, 0xa90153f3}, CodeWord{0x31350a4, 0x2a0903ea, 0xa9025bf5},
    CodeWord{0x31350a8, 0x1100054d, 0xaa0003f5}, CodeWord{0x31350ac, 0x6b0901bf, 0xaa0103f3},
    CodeWord{0x31350b0, 0x54000140, 0xaa1f03f4}, CodeWord{0x31350b4, 0x71000d5f, 0xa9027e7f},
    CodeWord{0x31350b8, 0x1a9fb1ad, 0xb4000235}, CodeWord{0x31350bc, 0x3100055f, 0xf94002a8},
    CodeWord{0x31350c0, 0x1a8db16a, 0xf9402508}, CodeWord{0x31350c4, 0x937b7d4d, 0xaa1503e0},
    CodeWord{0x31350c8, 0xf86d698d, 0xd63f0100}, CodeWord{0x31350cc, 0x394001ad, 0xaa0003f6},
    CodeWord{0x31350d0, 0x3500006d, 0xb4000176}, CodeWord{0x31350d4, 0x17fffff5, 0xd00100e8},
    CodeWord{0x31350d8, 0x2a0903ea, 0xf9476100}, CodeWord{0x31350dc, 0xb908b90a, 0xaa1603e1},
    CodeWord{0x31350e0, 0xf9400bf3, 0x977c18a8}, CodeWord{0x31350e4, 0xa8c27bfd, 0xaa0003f4},
    CodeWord{0x31350e8, 0xd50323ff, 0xb40000b4}, CodeWord{0x31350ec, 0x17ffffcb, 0xaa1503e0},
    CodeWord{0x31350f0, 0x52800020, 0xaa1403e1}, CodeWord{0x31350f4, 0x940004c3, 0x977e7bb3},
    CodeWord{0x31350f8, 0x3607ff40, 0xa9025a74}, CodeWord{0x31350fc, 0xb988ba68, 0x52800020},
    CodeWord{0x3135100, 0xf000f3a9, 0xaa1303e1}, CodeWord{0x3135104, 0x9114a129, 0xd2800802},
    CodeWord{0x3135108, 0xd37be908, 0xaa1f03e3}, CodeWord{0x313510c, 0xf8686928, 0xaa1f03e4},
    CodeWord{0x3135110, 0xf9046268, 0x943d2b9c}, CodeWord{0x3135114, 0x17fffff3, 0x2a0003f5},
    CodeWord{0x3135118, 0x00000000, 0xb40000b4}, CodeWord{0x313511c, 0x00000000, 0xd00100e8},
    CodeWord{0x3135120, 0xf9446008, 0xf9476900}, CodeWord{0x3135124, 0x39400108, 0xaa1403e1},
    CodeWord{0x3135128, 0x34000268, 0x977c189e}, CodeWord{0x313512c, 0xd503237f, 0x2a1503e0},
    CodeWord{0x3135130, 0xa9bd7bfd, 0xa9425bf5}, CodeWord{0x3135134, 0xf9000bf5, 0xa94153f3},
    CodeWord{0x3135138, 0xa9024ff4, 0xa8c37bfd}, CodeWord{0x313513c, 0x910003fd, 0xd65f03c0},
    CodeWord{0x3135140, 0xaa0103f4, 0xa9bd7bfd}, CodeWord{0x3135144, 0xaa0003f3, 0x910003fd},
    CodeWord{0x3135148, 0x976b9836, 0xa90153f3}, CodeWord{0x313514c, 0x52800061, 0xaa0003f3},
    CodeWord{0x3135150, 0x52800022, 0xaa0103f4}, CodeWord{0x3135154, 0x52800075, 0xa9027fff},
    CodeWord{0x3135158, 0xaa1403e0, 0xf9400268}, CodeWord{0x313515c, 0x974af1fd, 0xf9400908},
    CodeWord{0x3135160, 0xb90a8a75, 0xaa1303e0}, CodeWord{0x3135164, 0xa9424ff4, 0xd63f0100},
    CodeWord{0x3135168, 0xf9400bf5, 0xf9000a80}, CodeWord{0x313516c, 0xa8c37bfd, 0xf9400268},
    CodeWord{0x3135170, 0xd50323ff, 0xf9400d08}, CodeWord{0x3135174, 0xd65f03c0, 0xaa1303e0},
    CodeWord{0x3135178, 0x00000000, 0x910083e1}, CodeWord{0x313517c, 0x00000000, 0xd63f0100},
    CodeWord{0x3135180, 0xd65f03c0, 0xf94013e0}, CodeWord{0x3135184, 0x00000000, 0xaa1403e1},
    CodeWord{0x3135188, 0x00000000, 0x97ffffc4}, CodeWord{0x313518c, 0x00000000, 0x2a0003f3},
    CodeWord{0x3135190, 0xd503237f, 0x910083e0}, CodeWord{0x3135194, 0xa9be7bfd, 0x975e7827},
    CodeWord{0x3135198, 0xf9000bf3, 0x2a1303e0}, CodeWord{0x313519c, 0x910003fd, 0xa94153f3},
    CodeWord{0x31351a0, 0x396bc008, 0xa8c37bfd}, CodeWord{0x31351a4, 0x36000088, 0xd65f03c0},
    CodeWord{0x31351a8, 0xf9400bf3, 0x52801fec}, CodeWord{0x31351ac, 0xa8c27bfd, 0xd001b0a8},
    CodeWord{0x31351b0, 0xd65f0fff, 0xf9420108}, CodeWord{0x31351b4, 0xaa0003f3, 0xb40001c8},
    CodeWord{0x31351b8, 0x52a00080, 0xb9400909}, CodeWord{0x31351bc, 0x94000491, 0xf940090a},
    CodeWord{0x31351c0, 0x36000140, 0x2a1f03eb}, CodeWord{0x31351c4, 0xb948b668, 0x6b09017f},
    CodeWord{0x31351c8, 0x71000509, 0x54000122}, CodeWord{0x31351cc, 0x7100551f, 0xf86b594d},
    CodeWord{0x31351d0, 0x1a89c3e9, 0xf94005ad}, CodeWord{0x31351d4, 0x7100051f, 0xf94005ad},
    CodeWord{0x31351d8, 0x52800288, 0xeb0001bf}, CodeWord{0x31351dc, 0x1a89b108, 0x54000060},
    CodeWord{0x31351e0, 0xb908b668, 0x1100056b}, CodeWord{0x31351e4, 0x17fffff1, 0x17fffff8},
    CodeWord{0x31351e8, 0x52a00100, 0x2a0b03ec}, CodeWord{0x31351ec, 0x94000485, 0x2a0c03e0},
    CodeWord{0x31351f0, 0x360000c0, 0xd65f03c0}, CodeWord{0x31351f4, 0xb948b668, 0xa9b77bfd},
    CodeWord{0x31351f8, 0x71004d1f, 0x910003fd}, CodeWord{0x31351fc, 0x1a88c7e9, 0xa90153f3},
    CodeWord{0x3135200, 0x3100051f, 0xa9025bf5}, CodeWord{0x3135204, 0x17fffff5, 0xa90323e0},
    CodeWord{0x3135208, 0x52800200, 0xf90043f7}, CodeWord{0x313520c, 0x9400047d, 0x2a1f03f7},
    CodeWord{0x3135210, 0x36000080, 0xaa0103f3}, CodeWord{0x3135214, 0x52800048, 0xaa1003f5},
    CodeWord{0x3135218, 0xb908ca68, 0x97ffff7c}, CodeWord{0x313521c, 0x17ffffe3, 0x34000560},
    CodeWord{0x3135220, 0x52800040, 0xf9401be0}, CodeWord{0x3135224, 0x94000477, 0x97ffffe1},
    CodeWord{0x3135228, 0x37000180, 0x2a0003f6}, CodeWord{0x313522c, 0xf9400268, 0xb9406274},
    CodeWord{0x3135230, 0xaa1303e0, 0x7100129f}, CodeWord{0x3135234, 0xf9406908, 0x52800089},
    CodeWord{0x3135238, 0xd63f0100, 0x1a89b294}, CodeWord{0x313523c, 0x7100081f, 0x7100069f},
    CodeWord{0x3135240, 0x54fffb4b, 0x540002cb}, CodeWord{0x3135244, 0xf9400bf3, 0xf9400260},
    CodeWord{0x3135248, 0x52800100, 0xb4000220}, CodeWord{0x313524c, 0xa8c27bfd, 0x5289e889},
    CodeWord{0x3135250, 0xd50323ff, 0x72aae869}, CodeWord{0x3135254, 0x1400046b, 0x5280002a},
    CodeWord{0x3135258, 0x52800068, 0x29082be9}, CodeWord{0x313525c, 0xaa1303e0, 0xb9400a69},
    CodeWord{0x3135260, 0xb908ca68, 0xb9400e6a}, CodeWord{0x3135264, 0xf9400268, 0x12005d4a},
    CodeWord{0x3135268, 0xf9406508, 0x2a16614a}, CodeWord{0x313526c, 0xd63f0100, 0x29092be9},
    CodeWord{0x3135270, 0x910063a1, 0xa9057fff}, CodeWord{0x3135274, 0xf9000fa0, 0xa9067fff},
    CodeWord{0x3135278, 0xaa1303e0, 0xf9400a69}, CodeWord{0x313527c, 0x97fffb51, 0xa9077fe9},
    CodeWord{0x3135280, 0x91286668, 0x910103e1}, CodeWord{0x3135284, 0x79119a60, 0x97ffffaf},
    CodeWord{0x3135288, 0x39286e7f, 0x2a0002f7}, CodeWord{0x313528c, 0x7900011f, 0x91006273},
    CodeWord{0x3135290, 0x17ffffc6, 0x51000694}, CodeWord{0x3135294, 0x00000000, 0x17ffffea},
    CodeWord{0x3135298, 0x00000000, 0xf9401fe8}, CodeWord{0x313529c, 0x00000000, 0xd2c00029},
    CodeWord{0x31352a0, 0xd503237f, 0xaa1f03ea}, CodeWord{0x31352a4, 0xa9bd7bfd, 0x34000077},
    CodeWord{0x31352a8, 0xf9000bf5, 0xd2c00049}, CodeWord{0x31352ac, 0xa9024ff4, 0xd280004a},
    CodeWord{0x31352b0, 0x910003fd, 0xa9002909}, CodeWord{0x31352b4, 0xaa0103f4, 0xf94043f7},
    CodeWord{0x31352b8, 0xb948c801, 0xa94153f3}, CodeWord{0x31352bc, 0xaa0003f3, 0xa9425bf5},
    CodeWord{0x31352c0, 0x35000061, 0xa8c97bfd}, CodeWord{0x31352c4, 0x396bc268, 0xd65f03c0},
    CodeWord{0x31352c8, 0x36000188, 0xa94323e0}, CodeWord{0x31352cc, 0xd001b0a8, 0xaa1303e1},
    CodeWord{0x31352d0, 0x91104108, 0xaa1503f0}, CodeWord{0x31352d4, 0xf9400108, 0xf94043f7},
    CodeWord{0x31352d8, 0xf9402900, 0xa94153f3}, CodeWord{0x31352dc, 0x9409d239, 0xa9425bf5},
    CodeWord{0x31352e0, 0x52800081, 0xa8c97bfd}, CodeWord{0x31352e4, 0x52800022, 0xa95d2809},
    CodeWord{0x31352e8, 0x52800095, 0xd61f0200}, CodeWord{0x31352ec, 0xaa1403e0, 0xb0001270},
    CodeWord{0x31352f0, 0x974af198, 0x9111d210}, CodeWord{0x31352f4, 0xb90a8a75, 0x17ffffc0},
    CodeWord{0x31352f8, 0xa9424ff4, 0xb0001270}, CodeWord{0x31352fc, 0xf9400bf5, 0x912b5210},
    CodeWord{0x3135300, 0xa8c37bfd, 0x17ffffbd}, CodeWord{0x3135304, 0xd65f0fff, 0xb0001270},
    CodeWord{0x3135308, 0x00000000, 0x911a9210}, CodeWord{0x313530c, 0x00000000, 0x17ffffba},
    CodeWord{0x3135310, 0xd503237f, 0xa9b77bfd}, CodeWord{0x3135314, 0xa9be7bfd, 0x910003fd},
    CodeWord{0x3135318, 0xf9000bf3, 0xa90153f3}, CodeWord{0x313531c, 0x910003fd, 0xa9025bf5},
    CodeWord{0x3135320, 0x97650530, 0xa90323e0}, CodeWord{0x3135324, 0x7100081f, 0xa90863f7},
    CodeWord{0x3135328, 0x54000121, 0x2a1f03f8}, CodeWord{0x313532c, 0x52800040, 0xaa0103f3},
    CodeWord{0x3135330, 0x9408a458, 0x2a0203f5}, CodeWord{0x3135334, 0x2a2003e8, 0x2a0303f6},
    CodeWord{0x3135338, 0x92400100, 0x97ffff34}, CodeWord{0x313533c, 0xf9400bf3, 0x340005e0},
    CodeWord{0x3135340, 0xa8c27bfd, 0xf9401be0}, CodeWord{0x3135344, 0xd50323ff, 0x97ffff99},
    CodeWord{0x3135348, 0x176597e6, 0x2a0003f7}, CodeWord{0x313534c, 0x2a0003f3, 0xb941c274},
    CodeWord{0x3135350, 0x9763fc68, 0x7100129f}, CodeWord{0x3135354, 0x7100041f, 0x52800089},
    CodeWord{0x3135358, 0x54000061, 0x1a89b294}, CodeWord{0x313535c, 0xaa1f03e0, 0x7100069f},
    CodeWord{0x3135360, 0x17fffff7, 0x5400034b}, CodeWord{0x3135364, 0x2a1303e0, 0xf9400260},
    CodeWord{0x3135368, 0x17fffff2, 0xb40002a0}, CodeWord{0x313536c, 0x00000000, 0x5289e889},
    CodeWord{0x3135370, 0xd65f03c0, 0x72aae869}, CodeWord{0x3135374, 0x00000000, 0x5280004a},
    CodeWord{0x3135378, 0x00000000, 0x29082be9}, CodeWord{0x313537c, 0x00000000, 0x3940426a},
    CodeWord{0x3135380, 0xd503237f, 0x120002a9}, CodeWord{0x3135384, 0xa9be7bfd, 0x2a09414a},
    CodeWord{0x3135388, 0xa9014ff4, 0x2a17614a}, CodeWord{0x313538c, 0x910003fd, 0x29092bff},
    CodeWord{0x3135390, 0xaa0003f3, 0xf9400669}, CodeWord{0x3135394, 0xaa0103e0, 0xa90527ff},
    CodeWord{0x3135398, 0x528000a1, 0xa9067fff}, CodeWord{0x313539c, 0x52800022, 0xf9403669},
    CodeWord{0x31353a0, 0x528000b4, 0xb940166a}, CodeWord{0x31353a4, 0x974af16b, 0xb940626b},
    CodeWord{0x31353a8, 0xb90a8a74, 0xf9003be9}, CodeWord{0x31353ac, 0xa9414ff4, 0x290f2fea},
    CodeWord{0x31353b0, 0xa8c27bfd, 0x910103e1}, CodeWord{0x31353b4, 0xd65f0fff, 0x97ffff63},
    CodeWord{0x31353b8, 0x00000000, 0x2a000318}, CodeWord{0x31353bc, 0x00000000, 0x9101c273},
    CodeWord{0x31353c0, 0xd65f03c0, 0x51000694}, CodeWord{0x31353c4, 0x00000000, 0x17ffffe6},
    CodeWord{0x31353c8, 0x00000000, 0xf9401fe8}, CodeWord{0x31353cc, 0x00000000, 0xd2c00029},
    CodeWord{0x31353d0, 0xd65f03c0, 0xaa1f03ea}, CodeWord{0x31353d4, 0x00000000, 0x34000078},
    CodeWord{0x31353d8, 0x00000000, 0xd2c00049}, CodeWord{0x31353dc, 0x00000000, 0xd280004a},
    CodeWord{0x31353e0, 0xd503237f, 0xa9002909}, CodeWord{0x31353e4, 0xa9be7bfd, 0xa94863f7},
    CodeWord{0x31353e8, 0xa9014ff4, 0xa94153f3}, CodeWord{0x31353ec, 0x910003fd, 0xa9425bf5},
    CodeWord{0x31353f0, 0xb948c808, 0xa8c97bfd}, CodeWord{0x31353f4, 0x52800234, 0xd65f03c0},
    CodeWord{0x31353f8, 0xaa0003f3, 0xa94323e0}, CodeWord{0x31353fc, 0x340001a8, 0xaa1303e1},
    CodeWord{0x3135400, 0xb988ba6a, 0x2a1503e2}, CodeWord{0x3135404, 0x396bc268, 0x2a1603e3},
    CodeWord{0x3135408, 0xf000f3ab, 0xa94863f7}, CodeWord{0x313540c, 0x9114a16b, 0xa94153f3},
    CodeWord{0x3135410, 0x912b4269, 0xa9425bf5}, CodeWord{0x3135414, 0x8b0a156a, 0xa8c97bfd},
    CodeWord{0x3135418, 0x7100011f, 0xa95d2809}, CodeWord{0x313541c, 0x9a8a1128, 0x14093626},
    CodeWord{0x3135420, 0x39406508, 0xa9b87bfd}, CodeWord{0x3135424, 0x7100011f, 0x910003fd},
    CodeWord{0x3135428, 0x528000c8, 0xa90153f3}, CodeWord{0x313542c, 0x1a941114, 0xa90307e0},
    CodeWord{0x3135430, 0x52800022, 0xaa0103f3}, CodeWord{0x3135434, 0xaa0103e0, 0x2a0203f4},
    CodeWord{0x3135438, 0x2a1403e1, 0x97fffef4}, CodeWord{0x313543c, 0x974af145, 0x340001a0},
    CodeWord{0x3135440, 0xb90a8a74, 0x5289e889}, CodeWord{0x3135444, 0xa9414ff4, 0x72aae869},
    CodeWord{0x3135448, 0xa8c27bfd, 0x5280006a}, CodeWord{0x313544c, 0xd65f0fff, 0x29082be9},
    CodeWord{0x3135450, 0xd503237f, 0x12001e8a}, CodeWord{0x3135454, 0xd10103ff, 0x29092bff},
    CodeWord{0x3135458, 0xa9027bfd, 0xa9057fff}, CodeWord{0x313545c, 0xa9034ff4, 0xa9067fff},
    CodeWord{0x3135460, 0x910083fd, 0xa9077fff}, CodeWord{0x3135464, 0xb00100a8, 0xf9401260},
    CodeWord{0x3135468, 0xaa0003f3, 0x910103e1}, CodeWord{0x313546c, 0xf9406108, 0x97ffff0b},
    CodeWord{0x3135470, 0xf9400100, 0xa94307e0}, CodeWord{0x3135474, 0x97f73ad7, 0x2a1403e2},
    CodeWord{0x3135478, 0xb948ca74, 0xa94153f3}, CodeWord{0x313547c, 0xb801a3ff, 0xa8c87bfd},
    CodeWord{0x3135480, 0x390063ff, 0xd503237f}, CodeWord{0x3135484, 0x71000a9f, 0x141ec100},
    CodeWord{0x3135488, 0x1a9f17e8, 0xa9be7bfd}, CodeWord{0x313548c, 0x390043e8, 0x910003fd},
    CodeWord{0x3135490, 0x9763fc18, 0xf9000be0}, CodeWord{0x3135494, 0xb90017e0, 0x94000086},
    CodeWord{0x3135498, 0x71000a9f, 0x34000080}, CodeWord{0x313549c, 0x54000100, 0x52800020},
    CodeWord{0x31354a0, 0xf9400268, 0xa8c27bfd}, CodeWord{0x31354a4, 0xaa1303e0, 0xd65f03c0},
    CodeWord{0x31354a8, 0xf9406d08, 0xf9400be0}, CodeWord{0x31354ac, 0xd63f0100, 0xa8c27bfd},
    CodeWord{0x31354b0, 0x79519a68, 0xd503237f}, CodeWord{0x31354b4, 0x790037e0, 0x14091fb8},
    CodeWord{0x31354b8, 0x79003be8, 0xa9be7bfd}, CodeWord{0x31354bc, 0x975b1fb5, 0x910003fd},
    CodeWord{0x31354c0, 0xb0010068, 0xf9000be0}, CodeWord{0x31354c4, 0xf9401414, 0x9400007a},
    CodeWord{0x31354c8, 0xf9439908, 0x34000080}, CodeWord{0x31354cc, 0xf9400108, 0x52800000},
    CodeWord{0x31354d0, 0xf9404900, 0xa8c27bfd}, CodeWord{0x31354d4, 0x97e1464b, 0xd65f03c0},
    CodeWord{0x31354d8, 0x910023e8, 0xf9400be0}, CodeWord{0x31354dc, 0x910043e1, 0xa8c27bfd},
    CodeWord{0x31354e0, 0xaa1403e0, 0xd503237f}, CodeWord{0x31354e4, 0x940228f3, 0x14091fbc},
    CodeWord{0x31354e8, 0x294127e8, 0xd65f03c0}, CodeWord{0x31354ec, 0xa9427bfd, 0xa9007d1f},
    CodeWord{0x31354f0, 0xb90aa268, 0xd65f03c0}, CodeWord{0x31354f4, 0xb90aa669, 0xa9ba7bfd},
    CodeWord{0x31354f8, 0xa9434ff4, 0x910003fd}, CodeWord{0x31354fc, 0x910103ff, 0xa90153f3},
    CodeWord{0x3135500, 0xd65f0fff, 0xa9025bf5}, CodeWord{0x3135504, 0x00000000, 0xaa0103f4},
    CodeWord{0x3135508, 0x00000000, 0xf9400413}, CodeWord{0x313550c, 0x00000000, 0xf9400e68},
    CodeWord{0x3135510, 0xd65f03c0, 0xf9001fe8}, CodeWord{0x3135514, 0x00000000, 0xb9401275},
    CodeWord{0x3135518, 0x00000000, 0x91008276}, CodeWord{0x313551c, 0x00000000, 0x34000255},
    CodeWord{0x3135520, 0xd503237f, 0xf94002c8}, CodeWord{0x3135524, 0xa9bd7bfd, 0xf9001be8},
    CodeWord{0x3135528, 0xf9000bf5, 0xa94086c0}, CodeWord{0x313552c, 0xa9024ff4, 0x97fffec3},
    CodeWord{0x3135530, 0x910003fd, 0xa9047fe0}, CodeWord{0x3135534, 0xaa0103f4, 0xf9400288},
    CodeWord{0x3135538, 0xaa0003f3, 0xf9400108}, CodeWord{0x313553c, 0x94032fc1, 0xaa1403e0},
    CodeWord{0x3135540, 0x912a8261, 0x9100c3e1}, CodeWord{0x3135544, 0x9402fce7, 0x910103e2},
    CodeWord{0x3135548, 0x37000140, 0x9100e3e3}, CodeWord{0x313554c, 0xb94aa668, 0xd63f0100},
    CodeWord{0x3135550, 0x7100051f, 0x910103e0}, CodeWord{0x3135554, 0x540000e1, 0x975e7737},
    CodeWord{0x3135558, 0x528000e1, 0x910062d6}, CodeWord{0x313555c, 0x52800022, 0x510006b5},
    CodeWord{0x3135560, 0x528000f5, 0x17ffffef}, CodeWord{0x3135564, 0xaa1403e0, 0xa9425bf5},
    CodeWord{0x3135568, 0x974af0fa, 0xa94153f3}, CodeWord{0x313556c, 0xb90a8a75, 0xa8c67bfd},
    CodeWord{0x3135570, 0xa9424ff4, 0xd65f03c0}, CodeWord{0x3135574, 0xf9400bf5, 0xa9b87bfd},
    CodeWord{0x3135578, 0xa8c37bfd, 0x910003fd}, CodeWord{0x313557c, 0xd65f0fff, 0xa90153f3},
    CodeWord{0x3135580, 0xd503237f, 0xa9025bf5}, CodeWord{0x3135584, 0xa9be7bfd, 0xf9001bf7},
    CodeWord{0x3135588, 0xf9000bf3, 0xaa0103f3}, CodeWord{0x313558c, 0x910003fd, 0x7100805f},
    CodeWord{0x3135590, 0xb948c808, 0x5400082b}, CodeWord{0x3135594, 0xaa0003f3, 0xb9400268},
    CodeWord{0x3135598, 0x7100091f, 0x5289e889}, CodeWord{0x313559c, 0x54000081, 0x72aa6869},
    CodeWord{0x31355a0, 0x975b1f7c, 0x6b09011f}, CodeWord{0x31355a4, 0xf9400c00, 0x54000781},
    CodeWord{0x31355a8, 0x94023a7a, 0xd001b0a8}, CodeWord{0x31355ac, 0x91287268, 0xf9420117},
    CodeWord{0x31355b0, 0xf9400bf3, 0xb4000737}, CodeWord{0x31355b4, 0xa9007d1f, 0xb9400e69},
    CodeWord{0x31355b8, 0xa8c27bfd, 0xb9400aea}, CodeWord{0x31355bc, 0xd65f0fff, 0x6b0a013f},
    CodeWord{0x31355c0, 0xd65f03c0, 0x540006a2}, CodeWord{0x31355c4, 0x00000000, 0xf9400aea},
    CodeWord{0x31355c8, 0x00000000, 0xf8695954}, CodeWord{0x31355cc, 0x00000000, 0xb4000654},
    CodeWord{0x31355d0, 0xd503237f, 0xb9400669}, CodeWord{0x31355d4, 0xa9bd7bfd, 0x7100053f},
    CodeWord{0x31355d8, 0xf9000bf5, 0x54000120}, CodeWord{0x31355dc, 0xa9024ff4, 0x7100093f},
    CodeWord{0x31355e0, 0x910003fd, 0x54000260}, CodeWord{0x31355e4, 0xaa0103f4, 0x71000d3f},
    CodeWord{0x31355e8, 0xaa0003f3, 0x54000561}, CodeWord{0x31355ec, 0x975b1f69, 0x97fff7f1},
    CodeWord{0x31355f0, 0xf9401000, 0x14000029}, CodeWord{0x31355f4, 0x9402fd6b, 0x940260fb},
    CodeWord{0x31355f8, 0x360000e0, 0x14000027}, CodeWord{0x31355fc, 0x52800101, 0x10fff769},
    CodeWord{0x3135600, 0x52800022, 0x10fff7aa}, CodeWord{0x3135604, 0x52800115, 0x10fff74b},
    CodeWord{0x3135608, 0xaa1403e0, 0xa90427e9}, CodeWord{0x313560c, 0x974af0d1, 0xa9052fea},
    CodeWord{0x3135610, 0xb90a8a75, 0x910103e9}, CodeWord{0x3135614, 0xa9424ff4, 0xa9064fe9},
    CodeWord{0x3135618, 0xf9400bf5, 0xaa1403e0}, CodeWord{0x313561c, 0xa8c37bfd, 0xb9400a61},
    CodeWord{0x3135620, 0xd65f0fff, 0x910183e2}, CodeWord{0x3135624, 0x00000000, 0x94091f07},
    CodeWord{0x3135628, 0x00000000, 0x1400001b}, CodeWord{0x313562c, 0x00000000, 0x9102c280},
    CodeWord{0x3135630, 0x3928601f, 0x943d1484}, CodeWord{0x3135634, 0xd65f03c0, 0xb9401275},
    CodeWord{0x3135638, 0x00000000, 0x9100c276}, CodeWord{0x313563c, 0x00000000, 0x34000115},
    CodeWord{0x3135640, 0xd503237f, 0xf9405ec0}, CodeWord{0x3135644, 0xa9be7bfd, 0xf94062c1},
    CodeWord{0x3135648, 0xa9014ff4, 0x97fffe7c}, CodeWord{0x313564c, 0x910003fd, 0xa9057ec0},
    CodeWord{0x3135650, 0xd00100a8, 0x910342d6}, CodeWord{0x3135654, 0xaa0003f3, 0x510006b5},
    CodeWord{0x3135658, 0xf9454908, 0x17fffff9}, CodeWord{0x313565c, 0xf9400108, 0xaa1403e0},
    CodeWord{0x3135660, 0xf9402100, 0x91008261}, CodeWord{0x3135664, 0x9409b39b, 0x2a1f03e2},
    CodeWord{0x3135668, 0x36000180, 0x94091ed6}, CodeWord{0x313566c, 0x94032f75, 0xb9401275},
    CodeWord{0x3135670, 0x912a8261, 0x9100c276}, CodeWord{0x3135674, 0x9402fc9b, 0x340000d5},
    CodeWord{0x3135678, 0x36000100, 0x910142c0}, CodeWord{0x313567c, 0xf9400268, 0x975e76ed},
    CodeWord{0x3135680, 0xb948e274, 0x910342d6}, CodeWord{0x3135684, 0xaa1303e0, 0x510006b5},
    CodeWord{0x3135688, 0xf9407508, 0x17fffffb}, CodeWord{0x313568c, 0xd63f0100, 0x9102c280},
    CodeWord{0x3135690, 0x6b00029f, 0x943d1470}, CodeWord{0x3135694, 0x540000c1, 0x2a1f03e0},
    CodeWord{0x3135698, 0x52800028, 0xf9401bf7}, CodeWord{0x313569c, 0x39286268, 0xa9425bf5},
    CodeWord{0x31356a0, 0xa9414ff4, 0xa94153f3}, CodeWord{0x31356a4, 0xa8c27bfd, 0xa8c87bfd},
    CodeWord{0x31356a8, 0xd65f0fff, 0xd65f03c0}, CodeWord{0x31356ac, 0x52800020, 0xd001b0a9},
    CodeWord{0x31356b0, 0x94000354, 0xf9420129}, CodeWord{0x31356b4, 0x3707ff20, 0xb4000149},
    CodeWord{0x31356b8, 0x17fffffa, 0xb940012a}, CodeWord{0x31356bc, 0x00000000, 0xb940092b},
    CodeWord{0x31356c0, 0x39686008, 0x6b0b015f}, CodeWord{0x31356c4, 0x7100051f, 0x540000c2},
    CodeWord{0x31356c8, 0x540001e1, 0xf940092b}, CodeWord{0x31356cc, 0xd503237f, 0xf86a596b},
    CodeWord{0x31356d0, 0xa9be7bfd, 0xeb00017f}, CodeWord{0x31356d4, 0xa9014ff4, 0x54000041},
    CodeWord{0x31356d8, 0x910003fd, 0x17fffe4c}, CodeWord{0x31356dc, 0xaa0003f3, 0x2a1f03e0},
    CodeWord{0x31356e0, 0xaa0103e0, 0xd65f03c0}, CodeWord{0x407ffe4, 0x97ffffe9, 0x97c2d564},
    CodeWord{0x3135700, 0xd50323ff, 0xa9b77bfd}, CodeWord{0x3135704, 0xd65f03c0, 0x910003fd},
    CodeWord{0x3135708, 0x00000000, 0xa90153f3}, CodeWord{0x313570c, 0x00000000, 0xa9025bf5},
    CodeWord{0x3135710, 0xd503237f, 0xa90363f7}, CodeWord{0x3135714, 0xa9be7bfd, 0x9402e9cb},
    CodeWord{0x3135718, 0xf9000bf3, 0x97fffe3c}, CodeWord{0x313571c, 0x910003fd, 0x34000a80},
    CodeWord{0x3135720, 0xb90a141f, 0xd001b0a8}, CodeWord{0x3135724, 0xd00100a8, 0xf9420113},
    CodeWord{0x3135728, 0xaa0003f3, 0xb4000a33}, CodeWord{0x313572c, 0xf9454908, 0xb9400269},
    CodeWord{0x3135730, 0xf9400108, 0xb9401a6a}, CodeWord{0x3135734, 0xf9402100, 0x6b0a013f},
    CodeWord{0x3135738, 0x9409b366, 0x540009a2}, CodeWord{0x313573c, 0x12000008, 0xf9401268},
    CodeWord{0x3135740, 0x39284268, 0xf8695914}, CodeWord{0x3135744, 0xf9400bf3, 0xb4000954},
    CodeWord{0x3135748, 0xa8c27bfd, 0x2a0903f7}, CodeWord{0x313574c, 0xd65f0fff, 0xb9802a95},
    CodeWord{0x3135750, 0xd503237f, 0xf9401a96}, CodeWord{0x3135754, 0xa9be7bfd, 0xb40008d5},
    CodeWord{0x3135758, 0xa9014ff4, 0xf94006d8}, CodeWord{0x313575c, 0x910003fd, 0xb4000838},
    CodeWord{0x3135760, 0xaa0103f4, 0x39420308}, CodeWord{0x3135764, 0xaa0003f3, 0x7100051f},
    CodeWord{0x3135768, 0x975b1f0a, 0x540007c1}, CodeWord{0x313576c, 0xf9401000, 0xf9400708},
    CodeWord{0x3135770, 0x9402fd30, 0x39400108}, CodeWord{0x3135774, 0x39684268, 0x34000768},
    CodeWord{0x3135778, 0x370001a8, 0xf9400ac0}, CodeWord{0x313577c, 0xd001b0a8, 0xb4000720},
    CodeWord{0x3135780, 0x91104108, 0xb9407008}, CodeWord{0x3135784, 0xf9400108, 0x350006e8},
    CodeWord{0x3135788, 0xf9402100, 0x5289e889}, CodeWord{0x313578c, 0x9409b351, 0x72aae869},
    CodeWord{0x3135790, 0x39684269, 0x5280002a}, CodeWord{0x3135794, 0x12000008, 0x29082be9},
    CodeWord{0x3135798, 0x6b08013f, 0x528fbba9}, CodeWord{0x313579c, 0x54000080, 0x72a98d09},
    CodeWord{0x31357a0, 0x39284268, 0xb941630a}, CodeWord{0x31357a4, 0xbd401280, 0x12001d4a},
    CodeWord{0x31357a8, 0xbd0a1660, 0x3218014a}, CodeWord{0x31357ac, 0xa9414ff4, 0x2a17614a},
    CodeWord{0x31357b0, 0xa8c27bfd, 0x29092be9}, CodeWord{0x31357b4, 0xd65f0fff, 0xa9057fff},
    CodeWord{0x31357b8, 0x00000000, 0xa9067fff}, CodeWord{0x31357bc, 0x00000000, 0xa9077fff},
    CodeWord{0x31357c0, 0xd503237f, 0x910103e1}, CodeWord{0x31357c4, 0xa9bc7bfd, 0x97fffe5f},
    CodeWord{0x31357c8, 0xf9000bf7, 0x5289e889}, CodeWord{0x31357cc, 0xa90257f6, 0x72aae869},
    CodeWord{0x31357d0, 0xa9034ff4, 0x5280006a}, CodeWord{0x31357d4, 0x910003fd, 0x29082be9},
    CodeWord{0x31357d8, 0xd00100b7, 0xb941630a}, CodeWord{0x31357dc, 0xaa0003f3, 0x12001d4a},
    CodeWord{0x31357e0, 0xaa0203f5, 0x2a17614a}, CodeWord{0x31357e4, 0xaa0103f4, 0x29092bff},
    CodeWord{0x31357e8, 0xf9454af7, 0xf9400709}, CodeWord{0x31357ec, 0xf94002e8, 0xa9057fe9},
    CodeWord{0x31357f0, 0xf9402100, 0xa9067fff}, CodeWord{0x31357f4, 0x9409b337, 0xf9403f09},
    CodeWord{0x31357f8, 0x2a0003f6, 0xa9077fe9}, CodeWord{0x31357fc, 0x94032f11, 0x910203e0},
    CodeWord{0x3135800, 0x912a8261, 0x975e6d78}, CodeWord{0x3135804, 0x9402fc37, 0x91022300},
    CodeWord{0x3135808, 0x36000180, 0x910203e1}, CodeWord{0x313580c, 0x36000356, 0x94094f71},
    CodeWord{0x3135810, 0xbd4012a0, 0x910203e0}, CodeWord{0x3135814, 0xbd4a1661, 0xb00083a1},
    CodeWord{0x3135818, 0x1e213800, 0x91385421}, CodeWord{0x313581c, 0x1e269001, 0xb9831b02},
    CodeWord{0x3135820, 0x1e212000, 0x9772958c}, CodeWord{0x3135824, 0x5400028d, 0x910203e0},
    CodeWord{0x3135828, 0x528000a8, 0x900081c1}, CodeWord{0x313582c, 0x910073a0, 0x912bb421},
    CodeWord{0x3135830, 0xb9001fa8, 0xb9831f02}, CodeWord{0x3135834, 0x1400000f, 0x97729587},
    CodeWord{0x3135838, 0xb94aa668, 0x910203e0}, CodeWord{0x313583c, 0x7100051f, 0xb0008081},
    CodeWord{0x3135840, 0x54000121, 0x913ccc21}, CodeWord{0x3135844, 0xf94002e8, 0xb9832302},
    CodeWord{0x3135848, 0xf9402100, 0x97729582}, CodeWord{0x313584c, 0x9409b321, 0xf94043e0},
    CodeWord{0x3135850, 0x360001c0, 0x910103e1}, CodeWord{0x3135854, 0x52800155, 0x97fffe11},
    CodeWord{0x3135858, 0x52800141, 0x910203e0}, CodeWord{0x313585c, 0xaa1403e0, 0x975e7675},
    CodeWord{0x3135860, 0x1400000d, 0x910062d6}, CodeWord{0x3135864, 0x528000a8, 0xd10006b5},
    CodeWord{0x3135868, 0x910063a0, 0x17ffffbb}, CodeWord{0x313586c, 0xb9001ba8, 0xa94363f7},
    CodeWord{0x3135870, 0x94090280, 0xa9425bf5}, CodeWord{0x3135874, 0xa9434ff4, 0xa94153f3},
    CodeWord{0x3135878, 0xa94257f6, 0xa8c97bfd}, CodeWord{0x313587c, 0xf9400bf7, 0xd65f03c0},
    CodeWord{0x3135880, 0xa8c47bfd, 0xa9ba7bfd}, CodeWord{0x3135884, 0xd65f0fff, 0x910003fd},
    CodeWord{0x3135888, 0x52800175, 0xa90153f3}, CodeWord{0x313588c, 0xaa1403e0, 0xaa0003f3},
    CodeWord{0x3135890, 0x52800161, 0xaa0103f4}, CodeWord{0x3135894, 0x52800022, 0x97fffddd},
    CodeWord{0x3135898, 0x974af02e, 0x34000200}, CodeWord{0x313589c, 0xb90a8a75, 0x5289e889},
    CodeWord{0x31358a0, 0x17fffff5, 0x72aae869}, CodeWord{0x31358a4, 0x00000000, 0x5280008a},
    CodeWord{0x31358a8, 0x00000000, 0x29042be9}, CodeWord{0x31358ac, 0x00000000, 0xf9400289},
    CodeWord{0x31358b0, 0xd503237f, 0xa902a7ff}, CodeWord{0x31358b4, 0xd10483ff, 0xa903ffff},
    CodeWord{0x31358b8, 0xa90f7bfd, 0xa904ffff}, CodeWord{0x31358bc, 0xa91057fc, 0xf9002fff},
    CodeWord{0x31358c0, 0xa9114ff4, 0x52800020}, CodeWord{0x31358c4, 0x9103c3fd, 0x910083e1},
    CodeWord{0x31358c8, 0xaa0003f3, 0xd2800802}, CodeWord{0x31358cc, 0xf90027ff, 0xaa1f03e3},
    CodeWord{0x31358d0, 0x97689764, 0xaa1f03e4}, CodeWord{0x31358d4, 0xf9400008, 0x943d29ab},
    CodeWord{0x31358d8, 0xa941250a, 0xaa1303e0}, CodeWord{0x31358dc, 0xa9402d08, 0xaa1403e1},
    CodeWord{0x31358e0, 0xa9052fe8, 0xa94153f3}, CodeWord{0x31358e4, 0xa90627ea, 0xa8c67bfd},
    CodeWord{0x31358e8, 0xd001afe8, 0x1409661e}, CodeWord{0x3133424, 0x9402f287, 0x940008b7},
    CodeWord{0x31e3ca4, 0x9406ad2f, 0x97fd46f7},
};

constexpr std::array SwitchSportsResortRecvErrorWords{
    CodeWord{0x298c474, 0x97ffe2f3, 0x941ea553}, CodeWord{0x298d904, 0x97ffddcf, 0x941ea02f},
    CodeWord{0x31359c0, 0x72a15422, 0x2a1f03e0}, CodeWord{0x31359c4, 0x974b4f7f, 0xd503201f},
    CodeWord{0x31359c8, 0xb9400be1, 0xd65f03c0}, CodeWord{0x339d990, 0x97f896fc, 0x97f6600c},
    CodeWord{0x339e418, 0x97f8945a, 0x97f65d6a}, CodeWord{0x339e598, 0x97f893fa, 0x97f65d0a},
};

constexpr std::array SwitchSportsResortErrorFilterWords{
    CodeWord{0x3135a00, 0x71002ebf, 0xa9be07e0}, CodeWord{0x3135a04, 0x540000c0, 0xa9017be2},
    CodeWord{0x3135a08, 0x710032bf, 0xb9400021}, CodeWord{0x3135a0c, 0x54000141, 0x52800002},
    CodeWord{0x3135a10, 0xf9402288, 0x52800120}, CodeWord{0x3135a14, 0x529fbb89, 0xd40f4381},
    CodeWord{0x3135a18, 0x14000003, 0x2a0003e9}, CodeWord{0x3135a1c, 0xf9402288, 0xa9417be2},
    CodeWord{0x3135a20, 0x529e0989, 0xa8c207e0}, CodeWord{0x3135a24, 0x72a00909, 0x35000069},
    CodeWord{0x3135a28, 0xb8696901, 0xd503237f}, CodeWord{0x3135a2c, 0x910043e0, 0x140236d6},
    CodeWord{0x3135a30, 0x976089f8, 0x2a1f03e0}, CodeWord{0x3135a34, 0x910043e0, 0xd65f03c0},
    CodeWord{0x31c3580, 0xd503237f, 0x17fdc920}, CodeWord{0x3135a40, 0x9123a261, 0xa9be07e0},
    CodeWord{0x3135a44, 0xf9439908, 0xa9017be2}, CodeWord{0x3135a48, 0xf9400108, 0xb9400021},
    CodeWord{0x3135a4c, 0xf9404d00, 0x52800022}, CodeWord{0x3135a50, 0x910043e8, 0x52800120},
    CodeWord{0x3135a54, 0x97e1621b, 0xd40f4381}, CodeWord{0x3135a58, 0x910043e0, 0x2a0003e9},
    CodeWord{0x3135a5c, 0x910123e1, 0xa9417be2}, CodeWord{0x3135a60, 0x52801502, 0xa8c207e0},
    CodeWord{0x3135a64, 0x97e1550f, 0x35000069}, CodeWord{0x3135a68, 0xa9514ff4, 0xd503237f},
    CodeWord{0x3135a6c, 0xa95057fc, 0x17e13d76}, CodeWord{0x3135a70, 0xa94f7bfd, 0x2a1f03e0},
    CodeWord{0x3135a74, 0x910483ff, 0xd65f03c0}, CodeWord{0x2985040, 0xd503237f, 0x141ec280},
};

constexpr std::array SwitchSportsResortPlaySessionWords{
    CodeWord{0x3135a80, 0x52803689, 0xa9be7bfd}, CodeWord{0x3135a84, 0x72a00929, 0x910003fd},
    CodeWord{0x3135a88, 0x17ffffe8, 0xa90107e0}, CodeWord{0x3135a8c, 0x340000b5, 0x941f836d},
    CodeWord{0x3135a90, 0x710006bf, 0x71000c1f}, CodeWord{0x3135a94, 0x540001c0, 0xa94107e0},
    CodeWord{0x3135a98, 0x71000abf, 0xa8c27bfd}, CodeWord{0x3135a9c, 0x54fffcc1, 0x54000040},
    CodeWord{0x3135aa0, 0x910043e0, 0x176e45e0}, CodeWord{0x3135aa4, 0xaa1403e1, 0x52800029},
    CodeWord{0x3135aa8, 0x9400055a, 0x39034009}, CodeWord{0x3135aac, 0x17ffffca, 0x2a1f03e0},
    CodeWord{0x3135ab0, 0xf9402288, 0xd65f03c0}, CodeWord{0x3135ac0, 0xf9402288, 0xa9be7bfd},
    CodeWord{0x3135ac4, 0x52801889, 0x910003fd}, CodeWord{0x3135ac8, 0x17ffffef, 0xa90123e0},
    CodeWord{0x3135acc, 0xf9402288, 0x941f835d}, CodeWord{0x3135ad0, 0x910043e0, 0x71000c1f},
    CodeWord{0x3135ad4, 0x91523109, 0xa94123e0}, CodeWord{0x3135ad8, 0xb96ebd21, 0xa8c27bfd},
    CodeWord{0x3135adc, 0x976089cd, 0x5455cf60}, CodeWord{0x3135ae0, 0x910043e0, 0x6b08031f},
    CodeWord{0x3135ae4, 0xaa1403e1, 0x5455ce61}, CodeWord{0x3135ae8, 0x9400054a, 0x1402ae57},
    CodeWord{0x31e1090, 0x976b9864, 0x97fd527c}, CodeWord{0x31e4e44, 0x176b88f7, 0x17fd430f},
    CodeWord{0x31e143c, 0x6b08031f, 0x17fd51a1},
};

constexpr std::array SwitchSportsResortReadyWords{
    CodeWord{0x3135af0, 0xd65f03c0, 0xa9be7bfd}, CodeWord{0x3135af4, 0x00000000, 0x910003fd},
    CodeWord{0x3135af8, 0x00000000, 0xa90107e0}, CodeWord{0x3135afc, 0x00000000, 0x941f8351},
    CodeWord{0x3135b00, 0xd503237f, 0x71000c1f}, CodeWord{0x3135b04, 0xd10103ff, 0x54000201},
    CodeWord{0x3135b08, 0xa9017bfd, 0xd001b0a8}, CodeWord{0x3135b0c, 0xf90013f5, 0xf9420908},
    CodeWord{0x3135b10, 0xa9034ff4, 0xb40001a8}, CodeWord{0x3135b14, 0x910043fd, 0xf9402108},
    CodeWord{0x3135b18, 0xb0010068, 0xb4000168}, CodeWord{0x3135b1c, 0xaa0003f3, 0x5284d009},
    CodeWord{0x3135b20, 0xaa0103f4, 0x38696908}, CodeWord{0x3135b24, 0x9123a261, 0xa94107e0},
    CodeWord{0x3135b28, 0xf9439908, 0xa8c27bfd}, CodeWord{0x3135b2c, 0xf9400108, 0x6b08003f},
    CodeWord{0x3135b30, 0xf9404d00, 0x540000e0}, CodeWord{0x3135b34, 0x910003e8, 0xb200c3e9},
    CodeWord{0x3135b38, 0x97e161e2, 0x91040808}, CodeWord{0x3135b3c, 0x910003e0, 0xf9000109},
    CodeWord{0x3135b40, 0x97e154ec, 0xd65f03c0}, CodeWord{0x3135b44, 0x360000e0, 0xa94107e0},
    CodeWord{0x3135b48, 0x52800181, 0xa8c27bfd}, CodeWord{0x3135b4c, 0x52800022, 0x91040808},
    CodeWord{0x3135b50, 0x52800195, 0x1402bd61}, CodeWord{0x31e50d0, 0x91040808, 0x17fd4288},
};

constexpr std::array SwitchSportsResortStationMapWords{
    CodeWord{0x3136518, 0x54000220, 0xb9408844}, CodeWord{0x313651c, 0x2a1703e8, 0x7100049f},
    CodeWord{0x3136520, 0x9a9f3108, 0x54000620}, CodeWord{0x3136524, 0x910023e1, 0xb9401064},
    CodeWord{0x3136528, 0xaa1303e0, 0x7100049f}, CodeWord{0x313652c, 0xb8687ad5, 0x540005c1},
    CodeWord{0x3136530, 0x2a1503e2, 0xf001b1a4}, CodeWord{0x3136534, 0x974ce6ef, 0xf9430484},
    CodeWord{0x3136538, 0x910023e1, 0xb4000564}, CodeWord{0x313653c, 0x2a1503e2, 0xb9401885},
    CodeWord{0x3136540, 0xaa1403e3, 0x91002084}, CodeWord{0x3136544, 0x97e1240f, 0x710008bf},
    CodeWord{0x3136548, 0x370006e0, 0x54000062}, CodeWord{0x313654c, 0x71001aff, 0xf8655884},
    CodeWord{0x3136550, 0x1a98b2e8, 0x14000002}, CodeWord{0x3136554, 0x11000517, 0xf9400084},
    CodeWord{0x3136558, 0x17ffffed, 0xf9404c84}, CodeWord{0x313655c, 0xd2800108, 0xb4000444},
    CodeWord{0x3136560, 0x528000d8, 0xb9400c84}, CodeWord{0x3136564, 0x2a1f03f7, 0x71000c9f},
    CodeWord{0x3136568, 0xf2c00068, 0x540003e1}, CodeWord{0x313656c, 0xf90007e8, 0xf9402044},
    CodeWord{0x3136570, 0x71001aff, 0xb40003a4}, CodeWord{0x3136574, 0x540001e0, 0x52800005},
    CodeWord{0x3136578, 0x71001eff, 0x52800106}, CodeWord{0x313657c, 0x54000220, 0x3840148e},
    CodeWord{0x3136580, 0x2a1703e8, 0x5100c1cf}, CodeWord{0x3136584, 0x9a9f3108, 0x710025ff},
    CodeWord{0x3136588, 0x910023e1, 0x540000c9}, CodeWord{0x313658c, 0xaa1303e0, 0x321b01cf},
    CodeWord{0x3136590, 0xb8687ad5, 0x510185ef}, CodeWord{0x3136594, 0x2a1503e2, 0x710015ff},
    CodeWord{0x3136598, 0x974ce6d6, 0x54000268}, CodeWord{0x313659c, 0x910023e1, 0x110029ef},
    CodeWord{0x31365a0, 0x2a1503e2, 0x2a0511e5}, CodeWord{0x31365a4, 0xaa1403e3, 0x710004c6},
    CodeWord{0x31365a8, 0x97e123f6, 0x54fffea1}, CodeWord{0x31365ac, 0x370003c0, 0x3940008e},
    CodeWord{0x31365b0, 0x71001aff, 0x350001ae}, CodeWord{0x31365b4, 0x1a98b2e8, 0x5ac008a5},
    CodeWord{0x31365b8, 0x11000517, 0xb940046e}, CodeWord{0x31365bc, 0x17ffffed, 0x6b0e00bf},
    CodeWord{0x31365c0, 0xd2800208, 0x540000e1}, CodeWord{0x31365c4, 0x528000d8, 0x3942404e},
    CodeWord{0x31365c8, 0x2a1f03f7, 0x3940606f}, CodeWord{0x31365cc, 0xf2c00088, 0x6b0f01df},
    CodeWord{0x31365d0, 0xf90007e8, 0x54000061}, CodeWord{0x31365d4, 0x71001aff, 0x52800022},
    CodeWord{0x31365d8, 0x540001e0, 0xd65f03c0}, CodeWord{0x31365dc, 0x71001eff, 0x52800042},
    CodeWord{0x31365e0, 0x54000300, 0xd65f03c0}, CodeWord{0x31365e4, 0x2a1703e8, 0x52800002},
    CodeWord{0x31365e8, 0x9a9f3108, 0xd65f03c0}, CodeWord{0x31365ec, 0x910023e1, 0xaa1e03e7},
    CodeWord{0x31365f0, 0xaa1303e0, 0xaa0d03e2}, CodeWord{0x31365f4, 0xb8687ad5, 0xaa0103e3},
    CodeWord{0x31365f8, 0x2a1503e2, 0x97ffffc8}, CodeWord{0x31365fc, 0x974ce6bd, 0xaa0703fe},
    CodeWord{0x3136600, 0x910023e1, 0x7100045f}, CodeWord{0x3136604, 0x2a1503e2, 0x54000041},
    CodeWord{0x3136608, 0xaa1403e3, 0x140950cd}, CodeWord{0x313660c, 0x97e123dd, 0x7100085f},
    CodeWord{0x3136610, 0x370000a0, 0x54000041}, CodeWord{0x3136614, 0x71001aff, 0x140950af},
    CodeWord{0x3136618, 0x1a98b2e8, 0xb94089af}, CodeWord{0x313661c, 0x11000517, 0x140950b2},
    CodeWord{0x3136620, 0x17ffffed, 0xaa0003e2}, CodeWord{0x3136624, 0x52800020, 0x91004023},
    CodeWord{0x3136628, 0xa9444ff4, 0x97ffffbc}, CodeWord{0x313662c, 0xa94357f6, 0x34000062},
    CodeWord{0x3136630, 0xa9425ff8, 0x12000040}, CodeWord{0x3136634, 0xa9417bfd, 0x14094df1},
    CodeWord{0x3136638, 0x910143ff, 0xaa0003f3}, CodeWord{0x313663c, 0xd65f0fff, 0x14094df3},
    CodeWord{0x3389e04, 0xaa0003f3, 0x17f6b207}, CodeWord{0x338a8e0, 0xb94089af, 0x17f6af43},
};

constexpr std::array SwitchSportsResortMatchSeedWords{
    CodeWord{0x325b28c, 0x390063e8, 0x17fb6d3e}, CodeWord{0x32402b8, 0x394063e8, 0x17fbd95f},
    CodeWord{0x3136784, 0xaa1303e0, 0xa9bd07e0}, CodeWord{0x3136788, 0xaa1403e1, 0xa9010fe2},
    CodeWord{0x313678c, 0x390033e8, 0xd53b4201}, CodeWord{0x3136790, 0x9400000c, 0xf90013e1},
    CodeWord{0x3136794, 0x17ffffea, 0x35000448}, CodeWord{0x3136798, 0xa9484ff4, 0xf001b1a0},
    CodeWord{0x313679c, 0xa94757f6, 0xf9430400}, CodeWord{0x31367a0, 0xa9465ff8, 0xb40003e0},
    CodeWord{0x31367a4, 0xa94567fa, 0xb9401801}, CodeWord{0x31367a8, 0xa9447bfd, 0x91002000},
    CodeWord{0x31367ac, 0x910243ff, 0x7100083f}, CodeWord{0x31367b0, 0xd65f0fff, 0x54000062},
    CodeWord{0x31367b4, 0x00000000, 0xf8615800}, CodeWord{0x31367b8, 0x00000000, 0x14000002},
    CodeWord{0x31367bc, 0x00000000, 0xf9400000}, CodeWord{0x31367c0, 0xd503237f, 0xb40002e0},
    CodeWord{0x31367c4, 0xd10203ff, 0xf9404c00}, CodeWord{0x31367c8, 0xa9047bfd, 0xb40002a0},
    CodeWord{0x31367cc, 0xf9002bf7, 0xb9400c01}, CodeWord{0x31367d0, 0xa90657f6, 0x71000c3f},
    CodeWord{0x31367d4, 0xa9074ff4, 0x54000241}, CodeWord{0x31367d8, 0x910103fd, 0xd001b080},
    CodeWord{0x31367dc, 0xaa0203f4, 0xf9468000}, CodeWord{0x31367e0, 0xaa0003f3, 0xb40001e0},
    CodeWord{0x31367e4, 0x910023e0, 0x39428801}, CodeWord{0x31367e8, 0x9100a021, 0x36000061},
    CodeWord{0x31367ec, 0x52801002, 0xb940b001}, CodeWord{0x31367f0, 0x2a1f03e3, 0x14000008},
    CodeWord{0x31367f4, 0x941fddcb, 0xb001afe0}, CodeWord{0x31367f8, 0xb001afe8, 0xf946e800},
    CodeWord{0x31367fc, 0x91374108, 0xb4000100}, CodeWord{0x3136800, 0xf9400115, 0xf9402000},
    CodeWord{0x3136804, 0xb4000755, 0xb40000c0}, CodeWord{0x3136808, 0x9764fff6, 0x91523c00},
    CodeWord{0x313680c, 0x2a0003f6, 0xb94ddc01}, CodeWord{0x3136810, 0x97661a8c, 0x71004c3f},
    CodeWord{0x3136814, 0x360003c0, 0x54000048}, CodeWord{0x3136818, 0x910023e0, 0x32190028},
    CodeWord{0x313681c, 0x9760d1d5, 0xf94013e1}, CodeWord{0x3136820, 0x2a0003e1, 0xd51b4201},
    CodeWord{0x3136824, 0x52800020, 0xa9410fe2}, CodeWord{0x3136828, 0x9401f4ca, 0xa8c307e0},
    CodeWord{0x313682c, 0x910023e0, 0x390063e8}, CodeWord{0x3136830, 0x9760d218, 0x14049298},
    CodeWord{0x3136834, 0x2a0003e0, 0xa9bd07e0}, CodeWord{0x3136838, 0xaa1f03e1, 0xa9010fe2},
    CodeWord{0x313683c, 0x9401eef9, 0xd53b4201}, CodeWord{0x3136840, 0x910023e0, 0xf90013e1},
    CodeWord{0x3136844, 0x9760d213, 0x394123e2}, CodeWord{0x3136848, 0x52800021, 0x36380442},
    CodeWord{0x313684c, 0x2a0003e0, 0x12001842}, CodeWord{0x3136850, 0x9401eef4, 0x71004c5f},
    CodeWord{0x3136854, 0x71001adf, 0x540003e8}, CodeWord{0x3136858, 0x540006ad, 0xf001b1a0},
    CodeWord{0x313685c, 0x710026df, 0xf9430400}, CodeWord{0x3136860, 0x540007ed, 0xb4000380},
    CodeWord{0x3136864, 0x71002adf, 0xb9401801}, CodeWord{0x3136868, 0x54000b20, 0x91002000},
    CodeWord{0x313686c, 0x71002edf, 0x7100083f}, CodeWord{0x3136870, 0x54000960, 0x54000062},
    CodeWord{0x3136874, 0x710032df, 0xf8615800}, CodeWord{0x3136878, 0x540003a1, 0x14000002},
    CodeWord{0x313687c, 0x910023e0, 0xf9400000}, CodeWord{0x3136880, 0xaa1503e1, 0xb4000280},
    CodeWord{0x3136884, 0x94000057, 0xf9404c00}, CodeWord{0x3136888, 0x14000019, 0xb4000240},
    CodeWord{0x313688c, 0x910023e0, 0xb9400c01}, CodeWord{0x3136890, 0x941fde20, 0x71000c3f},
    CodeWord{0x3136894, 0x12001c08, 0x540001e1}, CodeWord{0x3136898, 0x7100051f, 0xd001b080},
    CodeWord{0x313689c, 0x1a9f17f7, 0xf9468000}, CodeWord{0x31368a0, 0x97688848, 0xb4000180},
    CodeWord{0x31368a4, 0x5296ef82, 0x39428801}, CodeWord{0x31368a8, 0x2a1703e1, 0x36000061},
    CodeWord{0x31368ac, 0x72ab5922, 0xb900b002}, CodeWord{0x31368b0, 0x9427af50, 0x14000008},
    CodeWord{0x31368b4, 0x910023e0, 0xb001afe0}, CodeWord{0x31368b8, 0x9760d1f6, 0xf946e800},
    CodeWord{0x31368bc, 0x2a0003f7, 0xb40000a0}, CodeWord{0x31368c0, 0x97688840, 0xf9402000},
    CodeWord{0x31368c4, 0x528b22c2, 0xb4000060}, CodeWord{0x31368c8, 0x2a1703e1, 0x91523c00},
    CodeWord{0x31368cc, 0x72a15422, 0xb90ddc02}, CodeWord{0x31368d0, 0x9427babc, 0xf94013e1},
    CodeWord{0x31368d4, 0x71002edf, 0xd51b4201}, CodeWord{0x31368d8, 0x54000620, 0xa9410fe2},
    CodeWord{0x31368dc, 0x710026df, 0xa8c307e0}, CodeWord{0x31368e0, 0x54000460, 0x394063e8},
    CodeWord{0x31368e4, 0x71001edf, 0x14042676},
};

constexpr std::array SwitchSportsResortAfterMatchWords{
    CodeWord{0x3133f70, 0xd503237f, 0xb903ba68}, CodeWord{0x3133f74, 0xa9be7bfd, 0xa9be7bfd},
    CodeWord{0x3133f78, 0xf9000bf3, 0x910003fd}, CodeWord{0x3133f7c, 0x910003fd, 0xa90157f4},
    CodeWord{0x3133f80, 0xaa0003f3, 0x2a0803f4}, CodeWord{0x3133f84, 0x97ffffbb, 0x941f8a2f},
    CodeWord{0x3133f88, 0xaa1303e0, 0x2a0003f5}, CodeWord{0x3133f8c, 0xf9400bf3, 0xd503201f},
    CodeWord{0x3133f90, 0x52816001, 0xd503201f}, CodeWord{0x3133f94, 0xa8c27bfd, 0xd503201f},
    CodeWord{0x3133f98, 0xd50323ff, 0xd503201f}, CodeWord{0x3133f9c, 0x173c30a1, 0xd503201f},
    CodeWord{0x3133fa0, 0xf9401808, 0xd503201f}, CodeWord{0x3133fa4, 0xb4000068, 0xd503201f},
    CodeWord{0x3133fa8, 0xb940ed00, 0xd503201f}, CodeWord{0x3133fac, 0xd65f03c0, 0x71000ebf},
    CodeWord{0x3133fb0, 0x2a1f03e0, 0x540002c1}, CodeWord{0x3133fb4, 0xd65f03c0, 0x71003e9f},
    CodeWord{0x3133fb8, 0x00000000, 0x54000281}, CodeWord{0x3133fbc, 0x00000000, 0x9001b0c8},
    CodeWord{0x3133fc0, 0xd503237f, 0xf9420908}, CodeWord{0x3133fc4, 0xd10283ff, 0xb4000228},
    CodeWord{0x3133fc8, 0xa9097bfd, 0xf9403514}, CodeWord{0x3133fcc, 0x910243fd, 0xb40001f4},
    CodeWord{0x3133fd0, 0xf9446c02, 0xaa1403e0}, CodeWord{0x3133fd4, 0xb948d003, 0x9409669f},
    CodeWord{0x3133fd8, 0xf9400024, 0xb4000180}, CodeWord{0x3133fdc, 0x910003e8, 0xaa0003f5},
    CodeWord{0x3133fe0, 0xf00083a1, 0xd503201f}, CodeWord{0x3133fe4, 0x91308c21, 0xd503201f},
    CodeWord{0x3133fe8, 0x910003e0, 0xd503201f}, CodeWord{0x3133fec, 0x91003108, 0xd503201f},
    CodeWord{0x3133ff0, 0x39022fff, 0xd503201f}, CodeWord{0x3133ff4, 0x390033ff, 0xd503201f},
    CodeWord{0x3133ff8, 0xf90003e8, 0xd503201f}, CodeWord{0x3133ffc, 0x52801008, 0xaa1503e1},
    CodeWord{0x3134000, 0xb9000be8, 0xaa1403e0}, CodeWord{0x3134004, 0x973bab4f, 0x9400061f},
    CodeWord{0x3134008, 0xf94003e8, 0xa94157f4}, CodeWord{0x313400c, 0xb9800be9, 0xa8c27bfd},
    CodeWord{0x3134010, 0x2a1f03e1, 0x910003e9}, CodeWord{0x3134014, 0x8b090108, 0x14082116},
    CodeWord{0x333c468, 0xb903ba68, 0x17f7dec2},
};

constexpr std::array SwitchSportsResortNextMatchWords{
    CodeWord{0x31368e8, 0x54000620, 0xa9be7bfd}, CodeWord{0x31368ec, 0x39400289, 0x910003fd},
    CodeWord{0x31368f0, 0x91287268, 0xa90107e0}, CodeWord{0x31368f4, 0x910023e0, 0x941f7fd3},
    CodeWord{0x31368f8, 0x8b09010a, 0x71000c1f}, CodeWord{0x31368fc, 0x7100413f, 0x54000801},
    CodeWord{0x3136900, 0x52800029, 0xf001b1a8}, CodeWord{0x3136904, 0x9a883148, 0xf9430508},
    CodeWord{0x3136908, 0x39286a69, 0xb40007a8}, CodeWord{0x313690c, 0x39000109, 0xb9401909},
    CodeWord{0x3136910, 0x941fdda8, 0xf940050a}, CodeWord{0x3136914, 0xa9474ff4, 0x7100053f},
    CodeWord{0x3136918, 0xa94657f6, 0x54000041}, CodeWord{0x313691c, 0xf9402bf7, 0xf940090a},
    CodeWord{0x3136920, 0xa9447bfd, 0xb40006ea}, CodeWord{0x3136924, 0x910203ff, 0xf940254a},
    CodeWord{0x3136928, 0xd65f0fff, 0xb40006aa}, CodeWord{0x313692c, 0x710006df, 0xf940e54a},
    CodeWord{0x3136930, 0x5400026c, 0xb500066a}, CodeWord{0x3136934, 0x34000456, 0x3945626c},
    CodeWord{0x3136938, 0x710006df, 0x528000eb}, CodeWord{0x313693c, 0x54fffd81, 0x2a1f03ea},
    CodeWord{0x3136940, 0x910023e0, 0x350004ac}, CodeWord{0x3136944, 0xaa1503e1, 0xf9408e69},
    CodeWord{0x3136948, 0x9400009e, 0xb4000449}, CodeWord{0x313694c, 0x910023e0, 0x3940012e},
    CodeWord{0x3136950, 0xaa1503e1, 0x7101c9df}, CodeWord{0x3136954, 0x940000b7, 0x54000201},
    CodeWord{0x3136958, 0x17ffffe5, 0x5280002d}, CodeWord{0x313695c, 0x71001edf, 0x386d692e},
    CodeWord{0x3136960, 0x54000260, 0x5100c1cf}, CodeWord{0x3136964, 0x710026df, 0x710029ff},
    CodeWord{0x3136968, 0x54fffc21, 0x540000c3}, CodeWord{0x313696c, 0x910023e0, 0x321b01ce},
    CodeWord{0x3136970, 0xaa1503e1, 0x51015dcf}, CodeWord{0x3136974, 0x9400015b, 0x510185d0},
    CodeWord{0x3136978, 0x17ffffdd, 0x71001a1f}, CodeWord{0x313697c, 0x71000adf, 0x540000c2},
    CodeWord{0x3136980, 0x540001e0, 0x2a0a11ea}, CodeWord{0x3136984, 0x710016df, 0x110005ad},
    CodeWord{0x3136988, 0x54fffb21, 0x710025bf}, CodeWord{0x313698c, 0x910023e0, 0x54fffe83},
    CodeWord{0x3136990, 0xaa1503e1, 0x14000002}, CodeWord{0x3136994, 0x9400005f, 0x2a1f03ea},
    CodeWord{0x3136998, 0x17ffffd5, 0x340001ca}, CodeWord{0x313699c, 0x910023e0, 0xb001b0a8},
    CodeWord{0x31369a0, 0xaa1503e1, 0xf9420108}, CodeWord{0x31369a4, 0x94000123, 0xb4000168},
    CodeWord{0x31369a8, 0x17ffffd1, 0xb9400109}, CodeWord{0x31369ac, 0x910023e0, 0xf9400908},
    CodeWord{0x31369b0, 0xaa1503e1, 0x5ac0094a}, CodeWord{0x31369b4, 0x940000f3, 0xf8695908},
    CodeWord{0x31369b8, 0x17ffffcd, 0xb40000c8}, CodeWord{0x31369bc, 0x910023e0, 0xf9400508},
    CodeWord{0x31369c0, 0xaa1503e1, 0xb4000088}, CodeWord{0x31369c4, 0x9400009b, 0xf940a108},
    CodeWord{0x31369c8, 0x17ffffc3, 0xb4000048}, CodeWord{0x31369cc, 0x910023e0, 0xb900090a},
    CodeWord{0x31369d0, 0xaa1503e1, 0x5280010b}, CodeWord{0x31369d4, 0x9400002f, 0xd503201f},
    CodeWord{0x31369d8, 0x17ffffc5, 0xd503201f}, CodeWord{0x31369dc, 0x00000000, 0xd503201f},
    CodeWord{0x31369e0, 0xd503237f, 0xd503201f}, CodeWord{0x31369e4, 0xa9be7bfd, 0xd503201f},
    CodeWord{0x31369e8, 0xa9014ff4, 0xf9400be0}, CodeWord{0x31369ec, 0x910003fd, 0x3903401f},
    CodeWord{0x31369f0, 0xf9402028, 0x2a0b03e1}, CodeWord{0x31369f4, 0x91523d14, 0xa8c27bfd},
    CodeWord{0x31369f8, 0x9760d1a6, 0x176e420a}, CodeWord{0x31369fc, 0xb90dde80, 0xa94107e0},
    CodeWord{0x3136a00, 0xf946ea80, 0xa8c27bfd}, CodeWord{0x3136a04, 0xb4000160, 0x17fffc1f},
    CodeWord{0x3136a08, 0x976887ea, 0x110006b5}, CodeWord{0x3136a0c, 0xaa0003f3, 0xd503201f},
    CodeWord{0x3136a10, 0x976887ec, 0xd503201f}, CodeWord{0x3136a14, 0xb94dde88, 0xd503201f},
    CodeWord{0x3136a18, 0x71004d1f, 0xd503201f}, CodeWord{0x3136a1c, 0x540001a8, 0x71001abf},
    CodeWord{0x3136a20, 0x900098c9, 0x54001803}, CodeWord{0x3136a24, 0x91277129, 0x140000dd},
    CodeWord{0x3136d48, 0x35000288, 0x35ffe608}, CodeWord{0x3136d58, 0xb4000208, 0xb4ffe588},
    CodeWord{0x3136d64, 0x340001aa, 0x34ffe52a}, CodeWord{0x31e1090, 0x97fd527c, 0x97fd5616},
};

constexpr std::array SwitchSportsResortRebuildErrorWords{
    CodeWord{0x3135adc, 0x5455cf60, 0x54ff9ac0}, CodeWord{0x3134e34, 0xf9400d08, 0x6b08031f},
    CodeWord{0x3134e38, 0xd63f0100, 0x54563482}, CodeWord{0x3134e3c, 0x9000f3c8, 0xa9be07e0},
    CodeWord{0x3134e40, 0x911fc108, 0xa9017be2}, CodeWord{0x3134e44, 0xaa1403e0, 0x2a1803e1},
    CodeWord{0x3134e48, 0xf90002a8, 0x2a0803e2}, CodeWord{0x3134e4c, 0xd503201f, 0x52800160},
    CodeWord{0x3134e50, 0x10004388, 0xd40f4381}, CodeWord{0x3134e54, 0xa907a293, 0x7100041f},
    CodeWord{0x3134e58, 0xf900469f, 0xa9417be2}, CodeWord{0x3134e5c, 0xa9424ff4, 0xa8c207e0},
    CodeWord{0x3134e60, 0xf9400bf5, 0x54563340}, CodeWord{0x3134e64, 0xa8c37bfd, 0x1402b193},
};

constexpr std::array SwitchSportsResortBotWords{
    CodeWord{0x2f0874c, 0x940b0461, 0x9408b928}, CodeWord{0x3136bec, 0xaa0003f3, 0xa9bf7bfd},
    CodeWord{0x3136bf0, 0x97688774, 0x910003fd}, CodeWord{0x3136bf4, 0xb94ebe81, 0x941f7f13},
    CodeWord{0x3136bf8, 0xb94eba83, 0x71000c1f}, CodeWord{0x3136bfc, 0xaa1303e2, 0xa8c17bfd},
    CodeWord{0x3136c00, 0xa9414ff4, 0x54000061}, CodeWord{0x3136c04, 0xa8c27bfd, 0x52800020},
    CodeWord{0x3136c08, 0xd50323ff, 0xd65f03c0}, CodeWord{0x3136c0c, 0x17658e89, 0x14024b31},
};

constexpr std::array SwitchSportsResortBotFillWords{
    CodeWord{0x31bcd30, 0x7100041f, 0x36000080}, CodeWord{0x31bcd34, 0x54000061, 0xd503201f},
};

constexpr std::array SwitchSportsResortBotOwnerWords{
    CodeWord{0x3137120, 0xb94af400, 0xa9bc7bfd}, CodeWord{0x3137124, 0xd65f03c0, 0x910003fd},
    CodeWord{0x3137128, 0x00000000, 0xa90107e0}, CodeWord{0x313712c, 0x00000000, 0xd101e002},
    CodeWord{0x3137130, 0xb988b80a, 0xb9400424}, CodeWord{0x3137134, 0x396bc008, 0xb90027e4},
    CodeWord{0x3137138, 0xb000f3ab, 0xb9401024}, CodeWord{0x313713c, 0x9114a16b, 0xb90033e4},
    CodeWord{0x3137140, 0x912b4009, 0x39424044}, CodeWord{0x3137144, 0x8b0a156a, 0x3900e3e4},
    CodeWord{0x3137148, 0x7100011f, 0x910083e3}, CodeWord{0x313714c, 0x9a8a1128, 0x97fffcf3},
    CodeWord{0x3137150, 0xb9400d00, 0x34000082}, CodeWord{0x3137154, 0xd65f03c0, 0x12000040},
    CodeWord{0x3137158, 0x00000000, 0xa8c47bfd}, CodeWord{0x313715c, 0x00000000, 0xd65f03c0},
    CodeWord{0x3137160, 0xf9400008, 0xa94107e0}, CodeWord{0x3137164, 0xf9406d01, 0x97e157a7},
    CodeWord{0x3137168, 0xd61f0020, 0xa8c47bfd}, CodeWord{0x313716c, 0x00000000, 0xd65f03c0},
    CodeWord{0x31ccec4, 0x97df004f, 0x97fda897},
};

constexpr std::array SwitchSportsResortLobbyRebuildWords{
    CodeWord{0x333d39c, 0x940f4691, 0x97f7e201}, CodeWord{0x3136934, 0x3945626c, 0x17fffd14},
    CodeWord{0x3134e34, 0x6b08031f, 0x14000402}, CodeWord{0x31defc4, 0x94072b6b, 0x97fd5b7c},
    CodeWord{0x31de9fc, 0x52800101, 0x17fd5d28}, CodeWord{0x3135b60, 0xa9434ff4, 0xd10043ff},
    CodeWord{0x3135b64, 0xf94013f5, 0x5289e889}, CodeWord{0x3135b68, 0xa9417bfd, 0x72aa2869},
    CodeWord{0x3135b6c, 0x910103ff, 0xb90003e9}, CodeWord{0x3135b70, 0xd65f0fff, 0x52800049},
    CodeWord{0x3135b74, 0x00000000, 0xb90007e9}, CodeWord{0x3135b78, 0x00000000, 0xb9000be0},
    CodeWord{0x3135b7c, 0x00000000, 0xb9000fff}, CodeWord{0x3135b80, 0xd65f03c0, 0x52800060},
    CodeWord{0x3135b84, 0x00000000, 0x910003e1}, CodeWord{0x3135b88, 0x00000000, 0xd2800202},
    CodeWord{0x3135b8c, 0x00000000, 0xaa1f03e3}, CodeWord{0x3135b90, 0xd65f03c0, 0xaa1f03e4},
    CodeWord{0x3135b94, 0x00000000, 0xd40f4381}, CodeWord{0x3135b98, 0x00000000, 0x910043ff},
    CodeWord{0x3135b9c, 0x00000000, 0xd65f03c0}, CodeWord{0x3135ba0, 0xd503237f, 0xa9bd7bfd},
    CodeWord{0x3135ba4, 0xd101c3ff, 0x910003fd}, CodeWord{0x3135ba8, 0xa9047bfd, 0xa90153f3},
    CodeWord{0x3135bac, 0xf9002bf5, 0xf90013f5}, CodeWord{0x3135bb0, 0xa9064ff4, 0x941f8324},
    CodeWord{0x3135bb4, 0x910103fd, 0x71000c1f}, CodeWord{0x3135bb8, 0x39686808, 0x54000de1},
    CodeWord{0x3135bbc, 0xaa0103f4, 0xd001b0a8}, CodeWord{0x3135bc0, 0xaa0003f3, 0xf9420908},
    CodeWord{0x3135bc4, 0x7100051f, 0xb4000d88}, CodeWord{0x3135bc8, 0x54000441, 0xf9402914},
    CodeWord{0x3135bcc, 0x52800028, 0xb4000d54}, CodeWord{0x3135bd0, 0xf9000fe8, 0xaa1403e0},
    CodeWord{0x3135bd4, 0x976896a3, 0x9409d047}, CodeWord{0x3135bd8, 0xf9400008, 0x36000ce0},
    CodeWord{0x3135bdc, 0x9123a261, 0xd001b2d3}, CodeWord{0x3135be0, 0xa941250a, 0x393c127f},
    CodeWord{0x3135be4, 0xa9402d08, 0x393c1a7f}, CodeWord{0x3135be8, 0xa9022fe8, 0x9001b1c8},
    CodeWord{0x3135bec, 0xa90327ea, 0xf9430508}, CodeWord{0x3135bf0, 0xb001ae88, 0xb4000c28},
    CodeWord{0x3135bf4, 0x91276108, 0xb9401909}, CodeWord{0x3135bf8, 0xf9400108, 0xf940050a},
    CodeWord{0x3135bfc, 0xf9404d00, 0x7100053f}, CodeWord{0x3135c00, 0x910023e8, 0x54000041},
    CodeWord{0x3135c04, 0x97e161af, 0xf940090a}, CodeWord{0x3135c08, 0x910023e0, 0xb4000b6a},
    CodeWord{0x3135c0c, 0x910063e1, 0xf940254a}, CodeWord{0x3135c10, 0x52800502, 0xb4000b2a},
    CodeWord{0x3135c14, 0x97e154a3, 0xf940e54a}, CodeWord{0x3135c18, 0x7200001f, 0xb400020a},
    CodeWord{0x3135c1c, 0x52800248, 0x52800029}, CodeWord{0x3135c20, 0x52800189, 0x393c1669},
    CodeWord{0x3135c24, 0xaa1403e0, 0xb90f027f}, CodeWord{0x3135c28, 0x1a881135, 0x52800020},
    CodeWord{0x3135c2c, 0x2a1503e1, 0x97ffffcd}, CodeWord{0x3135c30, 0x52800022, 0x7100081f},
    CodeWord{0x3135c34, 0x974aef47, 0x54000060}, CodeWord{0x3135c38, 0xb90a8a75, 0x393c1e7f},
    CodeWord{0x3135c3c, 0xa9464ff4, 0x1400004e}, CodeWord{0x3135c40, 0xf9402bf5, 0x397c1e69},
    CodeWord{0x3135c44, 0xa9447bfd, 0x7100313f}, CodeWord{0x3135c48, 0x9101c3ff, 0x54000962},
    CodeWord{0x3135c4c, 0xd65f0fff, 0x2a1f03f5}, CodeWord{0x3135c50, 0xd001b0a8, 0x52800021},
    CodeWord{0x3135c54, 0x91104108, 0x14000028}, CodeWord{0x3135c58, 0xf9400108, 0x397c1669},
    CodeWord{0x3135c5c, 0xf9402100, 0x340008c9}, CodeWord{0x3135c60, 0x9409b21c, 0xb94f0269},
    CodeWord{0x3135c64, 0x3607fec0, 0x11000529}, CodeWord{0x3135c68, 0x52800155, 0xb90f0269},
    CodeWord{0x3135c6c, 0x52800141, 0x7100793f}, CodeWord{0x3135c70, 0xaa1403e0, 0x5400082b},
    CodeWord{0x3135c74, 0x17ffffef, 0x2a1f03e0}, CodeWord{0x3135c78, 0x00000000, 0x97ffffba},
    CodeWord{0x3135c7c, 0x00000000, 0x34000700}, CodeWord{0x3135c80, 0xd65f03c0, 0x3100401f},
    CodeWord{0x3135c84, 0x00000000, 0x54000702}, CodeWord{0x3135c88, 0x00000000, 0x71000c1f},
    CodeWord{0x3135c8c, 0x00000000, 0x540006a0}, CodeWord{0x3135c90, 0xd65f03c0, 0x7100081f},
    CodeWord{0x3135c94, 0x00000000, 0x54000680}, CodeWord{0x3135c98, 0x00000000, 0x397c1e69},
    CodeWord{0x3135c9c, 0x00000000, 0x7100313f}, CodeWord{0x3135ca0, 0xd503237f, 0x54000662},
    CodeWord{0x3135ca4, 0xd101c3ff, 0x12802575}, CodeWord{0x3135ca8, 0xa9047bfd, 0x7100041f},
    CodeWord{0x3135cac, 0xf9002bf5, 0x54000061}, CodeWord{0x3135cb0, 0xa9064ff4, 0x528000e1},
    CodeWord{0x3135cb4, 0x910103fd, 0x14000010}, CodeWord{0x3135cb8, 0x52800028, 0xd001b0a8},
    CodeWord{0x3135cbc, 0xaa0103f3, 0xf9420108}, CodeWord{0x3135cc0, 0xaa0003f4, 0xb4000528},
    CodeWord{0x3135cc4, 0x390073a8, 0xb9400109}, CodeWord{0x3135cc8, 0xd001b0a8, 0xf9400908},
    CodeWord{0x3135ccc, 0x91104108, 0xf8695908}, CodeWord{0x3135cd0, 0xf9400108, 0xb40004a8},
    CodeWord{0x3135cd4, 0xf9402115, 0xf9400508}, CodeWord{0x3135cd8, 0xaa1503e0, 0xb4000468},
    CodeWord{0x3135cdc, 0x9409b1a1, 0xf940a108}, CodeWord{0x3135ce0, 0xf000f3a8, 0xb4000428},
    CodeWord{0x3135ce4, 0x91218108, 0xb9000900}, CodeWord{0x3135ce8, 0x910073a9, 0x52800029},
    CodeWord{0x3135cec, 0x910003e1, 0x393c1a69}, CodeWord{0x3135cf0, 0x390063a0, 0x52800101},
    CodeWord{0x3135cf4, 0xaa1503e0, 0xaa1403e0}, CodeWord{0x3135cf8, 0xa90027e8, 0x976e454a},
    CodeWord{0x3135cfc, 0x910063a8, 0x36000100}, CodeWord{0x3135d00, 0xa90153e8, 0x52800029},
    CodeWord{0x3135d04, 0x9409afd3, 0x393c1269}, CodeWord{0x3135d08, 0xf94003e8, 0x397c1e69},
    CodeWord{0x3135d0c, 0x910003e0, 0x11000529}, CodeWord{0x3135d10, 0xf9400d08, 0x393c1e69},
    CodeWord{0x3135d14, 0xd63f0100, 0x2a1503e9}, CodeWord{0x3135d18, 0x394073a8, 0x14000016},
    CodeWord{0x3135d1c, 0x7100051f, 0x397c1a69}, CodeWord{0x3135d20, 0x540000a1, 0x34000229},
    CodeWord{0x3135d24, 0x528001a1, 0x393c1a7f}, CodeWord{0x3135d28, 0xaa1303e0, 0xd001b0a8},
    CodeWord{0x3135d2c, 0x2a1f03e2, 0xf9420108}, CodeWord{0x3135d30, 0x974aef08, 0xb40001a8},
    CodeWord{0x3135d34, 0xa9464ff4, 0xb9400109}, CodeWord{0x3135d38, 0xf9402bf5, 0xf9400908},
    CodeWord{0x3135d3c, 0xa9447bfd, 0xf8695908}, CodeWord{0x3135d40, 0x9101c3ff, 0xb4000128},
    CodeWord{0x3135d44, 0xd65f0fff, 0xf9400508}, CodeWord{0x3135d48, 0x00000000, 0xb40000e8},
    CodeWord{0x3135d4c, 0x00000000, 0xf940a108}, CodeWord{0x3135d50, 0xd503237f, 0xb40000a8},
    CodeWord{0x3135d54, 0xa9bc7bfd, 0xb900091f}, CodeWord{0x3135d58, 0xf9000bf7, 0x14000003},
    CodeWord{0x3135d5c, 0xa90257f6, 0x393c167f}, CodeWord{0x3135d60, 0xa9034ff4, 0x393c1e7f},
    CodeWord{0x3135d64, 0x910003fd, 0x128003a9}, CodeWord{0x3135d68, 0xaa0003f3, 0x14000002},
    CodeWord{0x3135d6c, 0x975b1d89, 0x12800769}, CodeWord{0x3135d70, 0xf9400268, 0xb90f0269},
    CodeWord{0x3135d74, 0xf9401014, 0xf94013f5}, CodeWord{0x3135d78, 0xaa1303e0, 0xa94153f3},
    CodeWord{0x3135d7c, 0xf9406d08, 0xa8c37bfd}, CodeWord{0x3135d80, 0xd63f0100, 0x14176418},
    CodeWord{0x3135d84, 0xf9400268, 0xd001b2c9}, CodeWord{0x3135d88, 0x2a0003f5, 0x393c193f},
    CodeWord{0x3135d8c, 0xaa1303e0, 0x52800040}, CodeWord{0x3135d90, 0xf9407508, 0x97ffff74},
    CodeWord{0x3135d94, 0xd63f0100, 0x71000c1f}, CodeWord{0x3135d98, 0xf9400268, 0x540000a9},
    CodeWord{0x3135d9c, 0x2a0003f6, 0x3100401f}, CodeWord{0x3135da0, 0xaa1303e0, 0x54000062},
    CodeWord{0x3135da4, 0xf9407908, 0x5ac0080a}, CodeWord{0x3135da8, 0xd63f0100, 0x140002fd},
    CodeWord{0x3135dac, 0xf9400268, 0x3945626c}, CodeWord{0x3135db0, 0x2a0003f7, 0x140002e2},
    CodeWord{0x3135db4, 0xaa1303e0, 0xa9be7bfd}, CodeWord{0x3135db8, 0xf9407d08, 0x910003fd},
    CodeWord{0x3135dbc, 0xd63f0100, 0xf9000be0}, CodeWord{0x3135dc0, 0x910063a8, 0x941f82a0},
    CodeWord{0x3135dc4, 0x12000004, 0x71000c1f}, CodeWord{0x3135dc8, 0x52800025, 0x54000341},
    CodeWord{0x3135dcc, 0xaa1403e0, 0x2a1f03e0}, CodeWord{0x3135dd0, 0x2a1503e1, 0x97ffff64},
    CodeWord{0x3135dd4, 0x2a1603e2, 0xd001b2c9}, CodeWord{0x3135dd8, 0x2a1703e3, 0x393c153f},
    CodeWord{0x3135ddc, 0x9402fb2d, 0x397c112a}, CodeWord{0x3135de0, 0x294327a8, 0x3400028a},
    CodeWord{0x3135de4, 0xa94257f6, 0x393c113f}, CodeWord{0x3135de8, 0xf9400bf7, 0x9001b1c8},
    CodeWord{0x3135dec, 0xb90aa268, 0xf9430508}, CodeWord{0x3135df0, 0xb90aa669, 0xb4000208},
    CodeWord{0x3135df4, 0xa9434ff4, 0xb9401909}, CodeWord{0x3135df8, 0xa8c47bfd, 0xf940050a},
    CodeWord{0x3135dfc, 0xd65f0fff, 0x7100053f}, CodeWord{0x3135e00, 0xd65f03c0, 0x54000041},
    CodeWord{0x3135e04, 0x00000000, 0xf940090a}, CodeWord{0x3135e08, 0x00000000, 0xb400014a},
    CodeWord{0x3135e0c, 0x00000000, 0xf940254a}, CodeWord{0x3135e10, 0xd503237f, 0xb400010a},
    CodeWord{0x3135e14, 0xa9bd7bfd, 0xf940e54a}, CodeWord{0x3135e18, 0xf9000bf5, 0xb40000ca},
    CodeWord{0x3135e1c, 0xa9024ff4, 0xf9400be0}, CodeWord{0x3135e20, 0x910003fd, 0x52800021},
    CodeWord{0x3135e24, 0xaa0103f4, 0x976e44ff}, CodeWord{0x3135e28, 0xaa0003f3, 0xa8c27bfd},
    CodeWord{0x3135e2c, 0x94032d85, 0x1402a470}, CodeWord{0x3135e30, 0x912a8261, 0xf9400be0},
    CodeWord{0x3135e34, 0x9402faab, 0xa8c27bfd}, CodeWord{0x3135e38, 0x360000a0, 0x1409cfce},
    CodeWord{0x3135e3c, 0xa9424ff4, 0x6b08031f}, CodeWord{0x3135e40, 0xf9400bf5, 0x54ff7fe3},
    CodeWord{0x3135e44, 0xa8c37bfd, 0x3455b428}, CodeWord{0x3135e48, 0xd65f0fff, 0xaa0003f5},
    CodeWord{0x3135e4c, 0xb94aa668, 0x94032db9}, CodeWord{0x3135e50, 0x7100051f, 0xaa0003f6},
    CodeWord{0x3135e54, 0x54000101, 0x2a1f03f7}, CodeWord{0x3135e58, 0x528001c1, 0x2a1f03f9},
    CodeWord{0x3135e5c, 0x52800022, 0x910142a0}, CodeWord{0x3135e60, 0x528001d5, 0x2a1703e1},
    CodeWord{0x3135e64, 0xaa1403e0, 0x974c380b}, CodeWord{0x3135e68, 0x974aeeba, 0xd1008001},
    CodeWord{0x3135e6c, 0xb90a8a75, 0xaa1603e0}, CodeWord{0x3135e70, 0x17fffff3, 0x9402f210},
    CodeWord{0x3135e74, 0x528000a8, 0x394182a8}, CodeWord{0x3135e78, 0x910073a0, 0xf100001f},
    CodeWord{0x3135e7c, 0xb9001fa8, 0x110006f7}, CodeWord{0x3135e80, 0x940900fc, 0x1a990739},
    CodeWord{0x3135e84, 0x17ffffee, 0x6b0802ff}, CodeWord{0x3135e88, 0x00000000, 0x54fffea3},
    CodeWord{0x3135e8c, 0x00000000, 0x6b08033f}, CodeWord{0x3135e90, 0xd503237f, 0x5455b1c0},
    CodeWord{0x3135e94, 0xa9be7bfd, 0x3455b1b9}, CodeWord{0x3135e98, 0xf9000bf3, 0x17fffbe9},
    CodeWord{0x3135e9c, 0x910003fd, 0x941f8269}, CodeWord{0x3135ea0, 0xaa0003f3, 0x71000c1f},
    CodeWord{0x3135ea4, 0x975b1d3b, 0x540000c1}, CodeWord{0x3135ea8, 0xf9401000, 0x2a1f03e0},
    CodeWord{0x3135eac, 0x910063a8, 0x97ffff2d}, CodeWord{0x3135eb0, 0x9402fb1c, 0xd001b2c9},
    CodeWord{0x3135eb4, 0x294327a8, 0x393c153f}, CodeWord{0x3135eb8, 0xb90aa268, 0x393c193f},
    CodeWord{0x3135ebc, 0xb90aa669, 0x52800101}, CodeWord{0x3135ec0, 0xf9400bf3, 0x1402a2d0},
};

constexpr std::array BuiltInCodeFixes{
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortLanWords,
        .description = "Friend Lobby over LAN (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortJoinListWords,
        .description = "Friend Lobby join list from room members (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortLanJoinWords,
        .description = "Friend Lobby targeted LAN join (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortInviteIdGuardWords,
        .description =
            "Friend Lobby Invite ID search guards on LAN (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortHostOnWords,
        .description = "Friend Lobby hosting ON signal to Eden (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortHostOffWords,
        .description = "Friend Lobby hosting OFF signal to Eden (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortHuddleLeaveWords,
        .description = "Friend Lobby huddle leave stays local (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortHuddleNoHangWords,
        .description =
            "Friend Lobby huddle create and settings never hang (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortLobbyMaxWords,
        .description = "Friend Lobby player count shows 8 slots (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortBusPumpWords,
        .description = "Friend Lobby document bus pump (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortIdentityWords,
        .description = "Friend Lobby LAN identity and local member (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortDocBusWords,
        .description = "Friend Lobby gamesync document bus hooks (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortRecvErrorWords,
        .description =
            "Friend Lobby LAN receive errors stay silent (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortErrorFilterWords,
        .description =
            "Friend Lobby LAN-only errors filtered by Eden (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortPlaySessionWords,
        .description =
            "Friend Lobby LAN play session uses the lobby session (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortReadyWords,
        .description = "Friend Lobby LAN match ready handshake (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortStationMapWords,
        .description =
            "Friend Lobby LAN stations matched by address (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortMatchSeedWords,
        .description =
            "Friend Lobby LAN golf holes from the match master (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortAfterMatchWords,
        .description =
            "Friend Lobby LAN huddle left on return to the lobby (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortNextMatchWords,
        .description =
            "Friend Lobby LAN session rebuilt for the next match (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortRebuildErrorWords,
        .description = "Friend Lobby LAN next match waits for the huddle members' stations "
                       "(Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortBotWords,
        .description =
            "Friend Lobby LAN match master creates CPU players (Nintendo Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortBotFillWords,
        .description = "Friend Lobby LAN fills CPU player slots like online play (Nintendo Switch "
                       "Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortBotOwnerWords,
        .description = "Friend Lobby LAN matches CPU player owners by console address (Nintendo "
                       "Switch Sports Resort)",
    },
    BuiltInCodeFix{
        .build_id = "3941550A42845731651BCD05EDBB47031FFCCA26",
        .words = SwitchSportsResortLobbyRebuildWords,
        .description = "Friend Lobby LAN session rebuilt in the lobby after a match "
                       "(Nintendo Switch Sports Resort)",
    },
};

void ApplyBuiltInCodeFixes(const std::array<u8, 0x20>& build_id_bytes, std::span<u8> image) {
    const auto build_id_raw = Common::HexToString(build_id_bytes, true);
    const auto build_id = build_id_raw.substr(0, build_id_raw.find_last_not_of('0') + 1);
    std::map<u32, u32> patched;
    std::vector<std::string_view> applied;
    for (const auto& fix : BuiltInCodeFixes) {
        if (build_id != fix.build_id) {
            continue;
        }
        for (const auto& word : fix.words) {
            if (word.image_offset + sizeof(u32) > image.size()) {
                LOG_WARNING(Loader,
                            "Built-in fixes skipped: \"{}\" patches {:#x}, outside the image",
                            fix.description, word.image_offset);
                return;
            }
            u32 current{};
            if (const auto it = patched.find(word.image_offset); it != patched.end()) {
                current = it->second;
            } else {
                std::memcpy(&current, image.data() + word.image_offset, sizeof(current));
            }
            if (current != word.original) {
                LOG_WARNING(Loader,
                            "Built-in fixes skipped: \"{}\" found unexpected instruction {:#010x} "
                            "at {:#x}",
                            fix.description, current, word.image_offset);
                return;
            }
            patched.insert_or_assign(word.image_offset, word.replacement);
        }
        applied.push_back(fix.description);
    }
    for (const auto& [offset, value] : patched) {
        std::memcpy(image.data() + offset, &value, sizeof(value));
    }
    for (const auto description : applied) {
        LOG_INFO(Loader, "Applied built-in fix: {}", description);
    }
}
} // Anonymous namespace

bool NSOHeader::IsSegmentCompressed(size_t segment_num) const {
    ASSERT_MSG(segment_num < 3, "Invalid segment {}", segment_num);
    return ((flags >> segment_num) & 1) != 0;
}

AppLoader_NSO::AppLoader_NSO(FileSys::VirtualFile file_) : AppLoader(std::move(file_)) {}

FileType AppLoader_NSO::IdentifyType(const FileSys::VirtualFile& in_file) {
    u32 magic = 0;
    if (in_file->ReadObject(&magic) != sizeof(magic)) {
        return FileType::Error;
    }

    if (Common::MakeMagic('N', 'S', 'O', '0') != magic) {
        return FileType::Error;
    }

    return FileType::NSO;
}

std::optional<VAddr> AppLoader_NSO::LoadModule(Kernel::KProcess& process, Core::System& system, const FileSys::VfsFile& nso_file, VAddr load_base, bool should_pass_arguments, bool load_into_process, std::optional<FileSys::PatchManager> pm, std::vector<Core::NCE::Patcher>* patches, s32 patch_index) {
    if (nso_file.GetSize() < sizeof(NSOHeader))
        return std::nullopt;
    NSOHeader nso_header{};
    if (sizeof(NSOHeader) != nso_file.ReadObject(&nso_header))
        return std::nullopt;
    if (nso_header.magic != Common::MakeMagic('N', 'S', 'O', '0'))
        return std::nullopt;
    if (nso_header.segments.empty())
        return std::nullopt;

    // Allocate some space at the beginning if we are patching in PreText mode.
    const size_t module_start = [&]() -> size_t {
#ifdef HAS_NCE
        if (patches && load_into_process) {
            auto* patch = &patches->operator[](patch_index);
            if (patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
                return patch->GetSectionSize();
            } else if (patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
                return patch->GetPreSectionSize();
            }
        }
#endif
        return 0;
    }();

    auto const last_segment_it = &nso_header.segments[nso_header.segments.size() - 1];
    // Build program image directly in codeset memory :)
    Kernel::CodeSet codeset;
    codeset.memory.resize(module_start + last_segment_it->location + last_segment_it->size);
    {
        std::vector<u8> compressed_data(*std::ranges::max_element(nso_header.segments_compressed_size));
        std::vector<u8> decompressed_size(std::ranges::max_element(nso_header.segments, [](auto const& a, auto const& b) {
            return a.size < b.size;
        })->size);
        for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
            nso_file.Read(compressed_data.data(), nso_header.segments_compressed_size[i], nso_header.segments[i].offset);
            if (nso_header.IsSegmentCompressed(i)) {
                const u32 segment_size = nso_header.segments[i].size;
                if (nso_header.IsZBICCompressed()) {
                    // ZBIC compression
                    const int r = Common::Compression::DecompressDataZBIC(
                        std::span<u8>{decompressed_size}.first(segment_size),
                        std::span<const u8>{compressed_data}.first(nso_header.segments_compressed_size[i])
                    );
                    // A failed segment would leave the previous segment's bytes in the buffer and
                    // boot a corrupt image that hangs without presenting; refuse to load instead.
                    if (r <= 0) {
                        LOG_ERROR(Loader, "{}: segment {} failed ZBIC decompression",
                                  nso_file.GetName(), i);
                        return std::nullopt;
                    }
                    if (static_cast<u32>(r) != segment_size) {
                        LOG_WARNING(Loader, "{}: segment {} decompressed to {:#x} of {:#x} bytes",
                                    nso_file.GetName(), i, r, segment_size);
                        std::fill(decompressed_size.begin() + r,
                                  decompressed_size.begin() + segment_size, u8{0});
                    }
                } else {
                    // LZ4 compression
                    int r = Common::Compression::DecompressDataLZ4(
                        decompressed_size.data(),
                        segment_size,
                        compressed_data.data(),
                        nso_header.segments_compressed_size[i]
                    );
                    ASSERT(r == int(segment_size));
                }

                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    decompressed_size.data(),
                    nso_header.segments[i].size
                );
            } else {
                // Not compressed
                std::memcpy(
                    codeset.memory.data() + module_start + nso_header.segments[i].location,
                    compressed_data.data(),
                    nso_header.segments[i].size
                );
            }
            codeset.segments[i].addr = module_start + nso_header.segments[i].location;
            codeset.segments[i].offset = module_start + nso_header.segments[i].location;
            codeset.segments[i].size = nso_header.segments[i].size;
        }
    }

    if (should_pass_arguments && !Settings::values.program_args.GetValue().empty()) {
        const auto arg_data{Settings::values.program_args.GetValue()};

        codeset.DataSegment().size += NSO_ARGUMENT_DATA_ALLOCATION_SIZE;
        NSOArgumentHeader args_header{NSO_ARGUMENT_DATA_ALLOCATION_SIZE, static_cast<u32_le>(arg_data.size()), {}};
        const auto end_offset = codeset.memory.size();
        codeset.memory.resize(u32(codeset.memory.size()) + NSO_ARGUMENT_DATA_ALLOCATION_SIZE);
        std::memcpy(codeset.memory.data() + end_offset, &args_header, sizeof(NSOArgumentHeader));
        std::memcpy(codeset.memory.data() + end_offset + sizeof(NSOArgumentHeader), arg_data.data(), arg_data.size());
    }

    codeset.DataSegment().size += nso_header.segments[2].bss_size;
    u32 image_size = PageAlignSize(u32(codeset.memory.size()) + nso_header.segments[2].bss_size);
    codeset.memory.resize(image_size);

    for (std::size_t i = 0; i < nso_header.segments.size(); ++i) {
        codeset.segments[i].size = PageAlignSize(codeset.segments[i].size);
    }

    // Apply patches if necessary
    const auto name = nso_file.GetName();
    if (pm && (pm->HasNSOPatch(nso_header.build_id, name) || Settings::values.dump_nso)) {
        std::span<u8> patchable_section(codeset.memory.data() + module_start, codeset.memory.size() - module_start);
        std::vector<u8> pi_header(sizeof(NSOHeader) + patchable_section.size());
        std::memcpy(pi_header.data(), &nso_header, sizeof(NSOHeader));
        std::memcpy(pi_header.data() + sizeof(NSOHeader), patchable_section.data(),
                    patchable_section.size());

        pi_header = pm->PatchNSO(pi_header, name);

        std::copy(pi_header.begin() + sizeof(NSOHeader), pi_header.end(), patchable_section.data());
    }

    ApplyBuiltInCodeFixes(nso_header.build_id, std::span<u8>(codeset.memory.data() + module_start,
                                                             codeset.memory.size() - module_start));

    // Mario Kart World selects a 1- or 2-vblank frame mode in its main loop. Force its native
    // 30 FPS mode, or let it choose dynamically. In dynamic mode the game keeps a time account
    // (frame cost - interval * 16.67 ms) and advances 1 or 2 steps per frame, so it stays in real
    // time as long as the cost it reads is the real frame time: use the whole frame time and skip
    // the game's own wait for the second vblank, or every 2-step frame reads as 33 ms and the game
    // never leaves 30 FPS (found by SerkiGohan). Presents are then paced with interval 1.
    // Match the instruction sequences instead of relying on a title-version-specific address.
    // 60 FPS forces mode 1 (one 1/60 s step a frame): a slow frame plays in slow motion. With the
    // 30 FPS fallback, the mode load becomes a call to `ldr w8, <word>; ret` in the padding after
    // .text, a word nx2:vi2 switches between 1 and 2 (one 1/30 s step a frame) at runtime: 60
    // while frames make it, 30 at full speed while they don't.
    const bool mkw_dynamic = Settings::values.mkw_dynamic_fps.GetValue();
    const bool mkw_60 = !mkw_dynamic && Settings::values.mkw_60fps.GetValue();
    const bool mkw_60_switching = mkw_60 && Settings::values.mkw_60fps_fallback.GetValue();
    bool mkw_dynamic_fps_patch_applied = false;
    u64 mkw_mode_word_offset = 0;
    if ((mkw_dynamic || mkw_60 || Settings::values.mkw_30fps.GetValue()) && name == "main" &&
        process.GetProgramId() == Settings::MarioKartWorldProgramId) {
        constexpr u32 ModeLoadInsn = 0xB9421268; // ldr w8, [x19, #0x210]
        const u32 mode = mkw_dynamic ? 0U : mkw_60 ? 1U : 2U;
        const u32 mode_instruction = 0x52800008U | (mode << 5); // mov w8, #mode
        const auto& text = nso_header.segments[0];
        u8* const code = codeset.memory.data() + module_start + text.location;
        const size_t count = text.size / sizeof(u32);
        const auto word = [code](size_t index) {
            u32 value;
            std::memcpy(&value, code + index * sizeof(u32), sizeof(value));
            return value;
        };
        size_t mode_site = 0;
        size_t mode_matches = 0;
        for (size_t i = 2; i + 8 < count; ++i) {
            if (word(i) != ModeLoadInsn || word(i - 2) != 0x6B01013F || // cmp w9, w1
                (word(i - 1) & 0xFF00001F) != 0x54000001 ||             // b.ne
                (word(i + 1) & 0xFF00001F) != 0x35000008) {             // cbnz w8
                continue;
            }
            for (size_t j = i + 2; j < i + 8; ++j) {
                if (word(j) == 0x52A84E08) { // mov w8, #60.0f
                    mode_site = i;
                    ++mode_matches;
                    break;
                }
            }
        }

        size_t frame_time_site = 0;
        size_t frame_time_matches = 0;
        if (mode_matches == 1 && mkw_dynamic) {
            for (size_t i = mode_site + 1; i < mode_site + 32 && i < count; ++i) {
                if ((word(i) & 0xFFFFFFE0) == 0xF9409660) { // ldr xN, [x19, #0x128]
                    frame_time_site = i;
                    ++frame_time_matches;
                }
            }
        }

        // The vsync function waits until 0.99 * interval / 60 s after the frame start:
        // ldr w8, [x19, #0x88] ; cmp w8, #2 ; b.lo <done> ; ucvtf ; mov w8, #60.0f
        size_t vblank_wait_site = 0;
        size_t vblank_wait_matches = 0;
        if (mkw_dynamic) {
            for (size_t i = 0; i + 4 < count; ++i) {
                if (word(i) == 0xB9408A68 && word(i + 1) == 0x7100091F &&
                    (word(i + 2) & 0xFF00001F) == 0x54000003 && word(i + 4) == 0x52A84E08) {
                    vblank_wait_site = i + 2;
                    ++vblank_wait_matches;
                }
            }
        }

        // The padding between .text and the next segment is mapped with .text and is zero.
        const size_t cave = Common::AlignUp(text.location + text.size, sizeof(u32));
        const bool cave_fits =
            nso_header.segments.size() > 1 && cave + 12 <= nso_header.segments[1].location &&
            std::all_of(code + (cave - text.location), code + (cave - text.location) + 12,
                        [](u8 byte) { return byte == 0; });

        if (mode_matches == 1 && mkw_60_switching && cave_fits) {
            const std::array<u32, 3> stub{0x18000048U, // ldr w8, #8 (the mode word below)
                                          0xD65F03C0U, // ret
                                          1U};         // mode, switched by nx2:vi2
            std::memcpy(code + (cave - text.location), stub.data(), sizeof(stub));
            const s64 delta = (static_cast<s64>(cave) -
                               static_cast<s64>(text.location + mode_site * sizeof(u32))) / 4;
            const u32 call = 0x94000000U | (static_cast<u32>(delta) & 0x03FFFFFFU); // bl stub
            std::memcpy(code + mode_site * sizeof(u32), &call, sizeof(call));
            mkw_mode_word_offset = cave + 8;
            LOG_INFO(Loader, "Mario Kart World: 60 FPS mode enabled, 30 at full speed when slow "
                             "(main+{:#x}, mode word main+{:#x})",
                     text.location + mode_site * sizeof(u32), cave + 8);
        } else if (mode_matches == 1 && !mkw_60_switching &&
                   (!mkw_dynamic || (frame_time_matches == 1 && vblank_wait_matches == 1))) {
            std::memcpy(code + mode_site * sizeof(u32), &mode_instruction,
                        sizeof(mode_instruction));
            if (mkw_dynamic) {
                const u32 whole_frame_time = 0xF9407660 | (word(frame_time_site) & 0x1F);
                std::memcpy(code + frame_time_site * sizeof(u32), &whole_frame_time,
                            sizeof(whole_frame_time));
                // b.lo -> b to the same target.
                const s32 offset = static_cast<s32>(word(vblank_wait_site) << 8) >> 13;
                const u32 branch = 0x14000000U | (static_cast<u32>(offset) & 0x03FFFFFFU);
                std::memcpy(code + vblank_wait_site * sizeof(u32), &branch, sizeof(branch));
                mkw_dynamic_fps_patch_applied = true;
            }
            LOG_INFO(Loader, "Mario Kart World: {} FPS mode enabled (main+{:#x})",
                     mkw_dynamic ? "dynamic" : mkw_60 ? "60" : "30",
                     text.location + mode_site * sizeof(u32));
        } else {
            LOG_WARNING(Loader,
                        "Mario Kart World FPS patch skipped (mode matches {}, frame-time matches "
                        "{}, vblank-wait matches {}, room after .text {})",
                        mode_matches, frame_time_matches, vblank_wait_matches, cave_fits);
        }
    }

    // Hyrule Warriors' frame interval (core/hle/service/nx2_frame_interval.h). Its only copy into
    // the global is
    //   adrp x9, got; ...; ldr x9, [x9, #got]; ...; adrp x8, global; add x8, x8, #global;
    //   ldr w19, [x9]; str w19, [x8]
    // Nothing is patched; the present path writes both words with hyrule_dynamic_fps or
    // hyrule_30fps.
    u64 frame_interval_offset = 0;
    u64 frame_interval_source_offset = 0;
    if (name == "main" && process.GetProgramId() == Settings::HyruleWarriorsProgramId) {
        const auto& text = nso_header.segments[0];
        const u8* const image = codeset.memory.data() + module_start;
        const u64 image_end = codeset.memory.size() - module_start;
        const auto word = [image](u64 offset) {
            u32 value;
            std::memcpy(&value, image + offset, sizeof(value));
            return value;
        };
        const auto fits = [image_end](u64 offset, u64 size) {
            return image_end >= size && offset <= image_end - size;
        };
        const auto adrp_page = [](u64 pc, u32 insn) {
            s64 pages = static_cast<s64>((((insn >> 5) & 0x7FFFF) << 2) | ((insn >> 29) & 3));
            if (pages & (s64{1} << 20)) {
                pages -= s64{1} << 21;
            }
            return (pc & ~u64{0xFFF}) + static_cast<u64>(pages * 0x1000);
        };
        size_t matches = 0;
        for (u64 pc = text.location + 32; pc + 8 <= text.location + text.size; pc += 4) {
            if (word(pc) != 0xB9400133 || word(pc + 4) != 0xB9000113) { // ldr w19, [x9]; str w19, [x8]
                continue;
            }
            const u32 adrp_global = word(pc - 8);
            const u32 add_global = word(pc - 4);
            const u32 adrp_got = word(pc - 32);
            const u32 ldr_got = word(pc - 24);
            if ((adrp_global & 0x9F00001F) != 0x90000008 || (add_global & 0xFFC003FF) != 0x91000108 ||
                (adrp_got & 0x9F00001F) != 0x90000009 || (ldr_got & 0xFFC003FF) != 0xF9400129) {
                continue;
            }
            ++matches;
            frame_interval_offset = adrp_page(pc - 8, adrp_global) + ((add_global >> 10) & 0xFFF);
            const u64 got = adrp_page(pc - 32, adrp_got) + ((ldr_got >> 10) & 0xFFF) * 8;
            frame_interval_source_offset = 0;
            if (fits(got, sizeof(u64))) {
                // Not relocated yet: the entry still holds the module-relative address.
                std::memcpy(&frame_interval_source_offset, image + got, sizeof(u64));
            }
        }
        if (matches == 1 && fits(frame_interval_offset, sizeof(u32)) &&
            frame_interval_source_offset != 0 && fits(frame_interval_source_offset, sizeof(u32))) {
            LOG_INFO(Loader, "Hyrule Warriors frame interval at main+{:#x} (from main+{:#x})",
                     frame_interval_offset, frame_interval_source_offset);
        } else {
            LOG_WARNING(Loader, "Hyrule Warriors frame interval not found ({} matches)", matches);
            frame_interval_offset = frame_interval_source_offset = 0;
        }
    }

#ifdef HAS_NCE
    // If we are computing the process code layout and using nce backend, patch.
    const auto& code = codeset.CodeSegment();
    auto* patch = patches ? &patches->operator[](patch_index) : nullptr;
    if (patch && !load_into_process) {
        //Set module ID using build_id from the NSO header
        patch->SetModuleID(nso_header.build_id);
        // Patch SVCs and MRS calls in the guest code
        while (!patch->PatchText(codeset.memory, code)) {
            patch = &patches->emplace_back();
            patch->SetModuleID(nso_header.build_id);  // In case the patcher is changed for big modules, the new patcher should also have the build_id
        }
    } else if (patch) {
        // Relocate code patch and copy to the program image.
        // Save size before RelocateAndCopy (which may resize)
        const size_t size_before_relocate = codeset.memory.size();
        if (patch->RelocateAndCopy(load_base, code, codeset.memory, &process.GetPostHandlers())) {
            // Update patch section.
            auto& patch_segment = codeset.PatchSegment();
            auto& post_patch_segment = codeset.PostPatchSegment();
            const auto patch_mode = patch->GetPatchMode();
            if (patch_mode == Core::NCE::PatchMode::PreText) {
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else if (patch_mode == Core::NCE::PatchMode::Split) {
                // For Split-mode, we are using pre-patch buffer at start, post-patch buffer at end
                patch_segment.addr = 0;
                patch_segment.size = static_cast<u32>(patch->GetPreSectionSize());
                post_patch_segment.addr = size_before_relocate;
                post_patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            } else {
                patch_segment.addr = image_size;
                patch_segment.size = static_cast<u32>(patch->GetSectionSize());
            }
        }

        // Refresh image_size to take account the patch section if it was added by RelocateAndCopy
        image_size = static_cast<u32>(codeset.memory.size());
    }
#endif

    // If we aren't actually loading (i.e. just computing the process code layout), we are done
    if (!load_into_process) {
#ifdef HAS_NCE
        // Ok, so for Split mode, we need to account for pre-patch and post-patch space
        // which will be added during RelocateAndCopy in the second pass. Where it crashed
        // in Android Studio at PreText. May be a better way. Works for now.
        if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::Split) {
            return load_base + patch->GetPreSectionSize() + image_size + patch->GetSectionSize();
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PreText) {
            return load_base + patch->GetSectionSize() + image_size;
        } else if (patch && patch->GetPatchMode() == Core::NCE::PatchMode::PostData) {
            return load_base + image_size + patch->GetSectionSize();
        }
#endif
        return load_base + image_size;
    }

    // Apply cheats if they exist and the program has a valid title ID
    if (pm) {
        // TODO(Maufeat): Check if there is a better way to check
        if (name == "main")
            system.SetApplicationProcessBuildID(nso_header.build_id);

        const auto cheats = pm->CreateCheatList(nso_header.build_id);
        if (!cheats.empty()) {
            system.RegisterCheatList(cheats, nso_header.build_id, load_base, image_size);
        }
    }

    // Load codeset for current process
    if (name == "sdk") {
        const bool matching_sdk =
            Core::WindowTrace::Enabled() && process.GetProgramId() == Core::WindowTrace::ProgramId &&
            Common::HexToString(std::span{nso_header.build_id}.first(20)) ==
                "41D4DE4A3B78214AD20C896960DF0E5EA0ED595F";
        Core::WindowTrace::sessions.Reset(&process, matching_sdk ? load_base : 0);
        if (Core::WindowTrace::Enabled()) {
            const auto session = Core::WindowTrace::sessions.Find(&process);
            LOG_INFO(Loader, "Star Fox window trace: matching SDK={} base={:#x} session={}",
                     matching_sdk, matching_sdk ? load_base : 0, session ? session->id : 0);
        }
    }
    LOG_INFO(Loader, "Module {} loaded at {:#x} (size {:#x})", nso_file.GetName(), load_base, image_size);
    process.LoadModule(system.Kernel(), std::move(codeset), load_base);
    if (name == "main" && process.GetProgramId() == Settings::MarioKartWorldProgramId) {
        process.SetMkwDynamicFpsPatchApplied(mkw_dynamic_fps_patch_applied);
        process.SetMkwFrameModeAddress(
            mkw_mode_word_offset != 0 ? load_base + module_start + mkw_mode_word_offset : 0);
    }
    if (frame_interval_offset != 0) {
        process.SetFrameIntervalAddresses(load_base + module_start + frame_interval_offset,
                                          load_base + module_start + frame_interval_source_offset);
    }
    return load_base + image_size;
}

AppLoader_NSO::LoadResult AppLoader_NSO::Load(Kernel::KProcess& process, Core::System& system) {
    if (is_loaded) {
        return {ResultStatus::ErrorAlreadyLoaded, {}};
    }

    modules.clear();

    // Load module
    const VAddr base_address = GetInteger(process.GetEntryPoint());
    if (!LoadModule(process, system, *file, base_address, true, true)) {
        return {ResultStatus::ErrorLoadingNSO, {}};
    }

    modules.insert_or_assign(base_address, file->GetName());
    LOG_DEBUG(Loader, "loaded module {} @ {:#x}", file->GetName(), base_address);

    is_loaded = true;
    return {ResultStatus::Success, LoadParameters{Kernel::KThread::DefaultThreadPriority,
                                                  Core::Memory::DEFAULT_STACK_SIZE}};
}

ResultStatus AppLoader_NSO::ReadNSOModules(Modules& out_modules) {
    out_modules = this->modules;
    return ResultStatus::Success;
}

} // namespace Loader
