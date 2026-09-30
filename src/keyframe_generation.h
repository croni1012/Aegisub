// Copyright (c) 2026. SPDX-License-Identifier: BSD-3-Clause
#pragma once
namespace agi { struct Context; }
/// Run scene detection with a modal progress dialog, then load the result.
/// Return false on cancellation or failure; leave the active list untouched.
bool GenerateKeyframesFromVideo(agi::Context *context);
