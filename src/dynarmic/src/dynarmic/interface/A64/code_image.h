// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Dynarmic::A64 {

/// Translated guest code that outlives the process (UserConfig::code_image), so a program's
/// second run maps the code its earlier runs compiled instead of compiling it again.
///
/// A Jit with a code image records every block it compiles: the host code with every
/// position-dependent field and host address in it as a relocation (code-cache handlers,
/// constants, other blocks, executable addresses), the guest instructions it was translated
/// from, and the link, fault-recovery and exit-stub records the emitter keeps for it. On a
/// cache miss the Jit installs the image's block for that location instead of compiling,
/// after checking that guest memory still holds the same instructions. A block is keyed by
/// its location descriptor and a hash of the Jit's code-generation config, so Jits of one
/// process with different configs share one image without mixing code.
///
/// The serialised form is valid only for the same executable (its file contents are hashed),
/// the same host CPU features, the same format version and the same frontend key, which the
/// frontend sets to whatever identifies the guest program (title, module build IDs).
///
/// Thread-safe: several Jits (the cores of one process) can share one image, and Serialize can
/// run while they compile.
class CodeImage final {
public:
    explicit CodeImage(std::vector<std::uint8_t> frontend_key);
    ~CodeImage();
    CodeImage(const CodeImage&) = delete;
    CodeImage& operator=(const CodeImage&) = delete;

    /// Changes the frontend key Deserialize checks and Serialize writes. Blocks already in the
    /// image stay: whatever the key, a block is only installed over the same guest code.
    void SetFrontendKey(std::vector<std::uint8_t> frontend_key);

    /// Replaces the blocks with those of a serialised image. False, and no blocks, when
    /// `data` is not an image of this format, executable, host and frontend key, or its header
    /// is damaged. The image is a header and chunks of records with their own checksums: the
    /// records up to the first damaged or cut-off chunk are taken. `consumed`: the bytes of
    /// `data` that are whole (a file can be cut back to them before appending).
    bool Deserialize(std::span<const std::uint8_t> data, std::size_t* consumed = nullptr);
    /// The whole image.
    std::vector<std::uint8_t> Serialize() const;
    /// The records added since the last Serialize/SerializeAdded as a chunk to append to the
    /// image's serialised form (empty when there are none).
    std::vector<std::uint8_t> SerializeAdded();

    struct Statistics {
        std::size_t blocks = 0;               ///< blocks in the image
        std::uint64_t loaded = 0;             ///< blocks installed from the image instead of compiled
        std::uint64_t recorded = 0;           ///< compiled blocks added to the image
        std::uint64_t not_recordable = 0;     ///< compiled blocks the image cannot hold
        std::uint64_t guest_changed = 0;      ///< image blocks whose guest code differs in memory
        std::uint64_t not_installable = 0;    ///< image blocks that cannot be placed in this code cache
        std::uint64_t verify_mismatches = 0;  ///< UserConfig::code_image_verify failures
    };
    Statistics GetStatistics() const;
    /// Whether blocks were recorded since the last Deserialize/Serialize/SerializeAdded.
    bool Dirty() const;

    struct Impl;
    Impl& GetImpl() { return *impl; }

private:
    std::unique_ptr<Impl> impl;
};

}  // namespace Dynarmic::A64
