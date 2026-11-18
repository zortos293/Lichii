# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake -DROOT=<src> -DLIST=<file of paths under ROOT, one a line> -DOUT=<header> -P recompile_stamp.cmake
# The stamp of the recompiler's translations (nvn_recompile.c vk_recompiled.bin):
# a hash of the sources whose change can change a translation's SPIR-V or descriptors. The header
# is rewritten only when the stamp changes, so a build that touches none of them keeps the file.
file(STRINGS "${LIST}" paths)
set(all "")
foreach(path IN LISTS paths)
    file(SHA256 "${ROOT}/${path}" hash)
    string(APPEND all "${path} ${hash}\n")
endforeach()
string(SHA256 total "${all}")
string(SUBSTRING "${total}" 0 16 stamp)
set(text "#define NVN_RECOMPILE_STAMP 0x${stamp}ull\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
    file(WRITE "${OUT}" "${text}")
endif()
