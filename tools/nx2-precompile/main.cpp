// SPDX-License-Identifier: GPL-3.0-or-later
// nx2-precompile: the command-line front of video_core/nx2_precompile (see tools/nx2-precompile/README.md).

#include "video_core/nx2_precompile.h"

int main(int argc, char** argv) {
    return VideoCommon::Nx2Precompile::RunTool(argc, argv);
}
