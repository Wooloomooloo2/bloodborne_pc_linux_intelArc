// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport shader backend: IR program to SPIR-V.
#pragma once

#include <vector>

#include "shader/backend/bindings.h"
#include "shader/ir/program.h"
#include "shader/profile.h"

namespace Shader::Backend::SPIRV {

/// The module; `binding` advances past the program's bindings (stages are laid out in order).
[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info,
                                         const IR::Program& program, Bindings& binding);

/// BB_SPIRV_PRECISE: where float adds, multiplies and FMAs are NoContraction (0 vertex stages,
/// the default; 1 all stages; 2 none). Part of the shader cache version
/// (PipelineStore::ShaderBinaryVersion), so switching it rebuilds the cached SPIR-V.
int PreciseMode();

} // namespace Shader::Backend::SPIRV
