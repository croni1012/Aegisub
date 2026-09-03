// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <atomic>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <libaegisub/fs.h>

namespace ai {

class Error final : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

struct SubtitleLine {
	int id = 0;
	int start_ms = 0;
	int end_ms = 0;
	std::string source_text;
	std::string current_text;
	std::string actor;
	std::string style;
	std::string ass_text;
	bool target = false;
};

struct ProofreadIssue {
	int line_id = 0;
	std::vector<std::string> categories;
	std::string explanation;
	std::vector<std::string> suggestions;
};

struct ProofreadResult {
	std::string message;
	std::vector<ProofreadIssue> issues;
};

enum class KaraokeMode {
	AudioRecognition,
	SyllableTiming,
	KanjiGeneration
};

struct TimedTranscriptUnit {
	int start_ms = 0;
	int end_ms = 0;
	std::string text;
};

struct TimedTranscript {
	std::string text;
	std::vector<TimedTranscriptUnit> units;
};

struct KaraokeInputLine {
	int id = 0;
	int start_ms = 0;
	int end_ms = 0;
	std::string text;
};

struct KaraokeSyllable {
	int start_ms = 0;
	int end_ms = 0;
	std::string text;
	std::string kanji;
	std::string romaji;
	std::string language;
};

struct KaraokeLine {
	int source_line_id = 0;
	int start_ms = 0;
	int end_ms = 0;
	std::string kanji;
	std::string romaji;
	std::vector<KaraokeSyllable> syllables;
};

struct KaraokeResult {
	std::vector<KaraokeLine> lines;
};

struct LineReview {
	int id = 0;
	std::string japanese;
	std::string romaji;
	std::string verdict;
	std::string assessment;
	std::vector<std::string> issues;
	std::string suggested_text;
};

struct ReviewResult {
	std::string message;
	std::vector<LineReview> lines;
	/// Serialized Responses API input items. Kept only for the lifetime of the
	/// modal conversation and resent with store=false for follow-up turns.
	std::string conversation_json = "[]";
};

/// The API base URL used when nothing is configured.
std::string DefaultApiBase();

class OpenAIClient final {
	std::string api_key;
	std::string model;
	std::string transcription_model;
	std::string custom_instructions;
	std::atomic_bool *cancelled = nullptr;

public:
	OpenAIClient(std::string api_key, std::string model,
		std::string transcription_model, std::string custom_instructions = {},
		std::atomic_bool *cancelled = nullptr);

	/// Test the connection against the given base URL. An empty value means
	/// DefaultApiBase().
	void TestConnection(std::string const& base_url) const;
	std::string Transcribe(agi::fs::path const& audio_file) const;
	TimedTranscript TranscribeTimed(agi::fs::path const& audio_file) const;
	KaraokeResult CreateKaraoke(KaraokeMode mode,
		std::vector<KaraokeInputLine> const& lines,
		TimedTranscript const& transcript, bool advanced_timing) const;
	KaraokeResult CreateKanji(std::vector<KaraokeInputLine> const& lines) const;
	ReviewResult Review(std::vector<SubtitleLine> const& lines,
		std::string const& japanese_transcript) const;
	ReviewResult Continue(ReviewResult const& previous,
		std::string const& user_message) const;
	ProofreadResult Proofread(std::vector<SubtitleLine> const& lines) const;
};

/// Edit an image with a transparent PNG mask and return the base64-encoded PNG.
std::string EditImage(std::string const& api_key, std::string const& image_model,
	std::vector<unsigned char> const& image_png,
	std::vector<unsigned char> const& mask_png, std::string const& size,
	std::string const& prompt, std::function<bool()> const& is_cancelled = {});

struct CloudinaryCredentials {
	std::string cloud_name;
	std::string api_key;
	std::string api_secret;
	bool Complete() const { return !cloud_name.empty() && !api_key.empty() && !api_secret.empty(); }
};

std::string CloudinaryRemoveBackground(CloudinaryCredentials const& credentials,
	std::vector<unsigned char> const& image_png,
	std::function<bool()> const& is_cancelled = {});
std::string CloudinaryGenerativeRemove(CloudinaryCredentials const& credentials,
	std::vector<unsigned char> const& image_png, std::string const& prompt,
	std::function<bool()> const& is_cancelled = {});
void TestCloudinaryConnection(CloudinaryCredentials const& credentials);

std::string GetCloudinarySecret();
void SetSessionCloudinarySecret(std::string secret);
void ClearSessionCloudinarySecret();
bool StoreCloudinarySecret(std::string const& secret, std::string *error = nullptr);
bool DeleteStoredCloudinarySecret(std::string *error = nullptr);
bool HasStoredCloudinarySecret();

enum class ApiKeySource {
	None,
	Session,
	Environment,
	CredentialManager
};

/// The session key takes precedence, followed by OPENAI_API_KEY and the OS
/// credential store. The returned value must never be logged.
std::string GetApiKey();
ApiKeySource GetApiKeySource();
void SetSessionApiKey(std::string key);
void ClearSessionApiKey();
bool StoreApiKey(std::string const& key, std::string *error = nullptr);
bool DeleteStoredApiKey(std::string *error = nullptr);
bool HasStoredApiKey();

/// The language the review and the post-check work in, as an English language
/// name because the prompts embed it verbatim. Empty until one has been chosen.
std::string GetCheckLanguage();
void SetCheckLanguage(std::string language);
/// Languages offered when asking for one, English names in the same form.
std::vector<std::string> CheckLanguageChoices();

} // namespace ai
