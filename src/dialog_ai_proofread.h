// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "ai_client.h"

#include <vector>

class AssDialogue;
namespace agi { struct Context; }

/// Runs a modal Hungarian subtitle post-check and walks through every AI
/// suggestion one at a time. Aegisub remains blocked until the walk-through
/// is complete or the initial network request is cancelled.
void ShowAIProofreadDialog(agi::Context *context,
	std::vector<AssDialogue *> target_lines,
	std::vector<ai::SubtitleLine> context_lines);

/// Whether the current subtitle has a completed AI analysis which can be
/// reviewed again without making another network request.
bool HasLatestAIProofread(agi::Context const *context);

/// Replays the most recent AI post-check for the current subtitle without an
/// API key or an active AI connection.
void ShowLatestAIProofreadDialog(agi::Context *context);
