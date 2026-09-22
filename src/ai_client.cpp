// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "ai_client.h"

#include "format.h"
#include "options.h"

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <curl/curl.h>

#include <wx/base64.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <sstream>
#include <utility>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#elif defined(__APPLE__)
#include <Security/Security.h>
#elif defined(WITH_LIBSECRET)
#include <libsecret/secret.h>
#endif

namespace ai {
namespace {

constexpr char default_api_base[] = "https://api.openai.com/v1";

bool has_scheme(std::string const& url, char const *scheme) {
	auto const length = std::strlen(scheme);
	return url.size() >= length &&
		std::equal(scheme, scheme + length, url.begin(), [](char a, char b) {
			return a == std::tolower(static_cast<unsigned char>(b));
		});
}

std::string normalize_api_base(std::string base) {
	auto const first = base.find_first_not_of(" \t\r\n");
	if (first == std::string::npos)
		base.clear();
	else
		base = base.substr(first, base.find_last_not_of(" \t\r\n") - first + 1);

	if (base.empty()) base = default_api_base;

	// The API key travels to this address in an Authorization header, so assume
	// TLS when the user leaves the scheme out and refuse anything but HTTP(S).
	if (base.find("://") == std::string::npos)
		base.insert(0, "https://");
	else if (!has_scheme(base, "http://") && !has_scheme(base, "https://"))
		throw Error("Az API alapcímének http:// vagy https:// protokollt kell használnia.");

	while (base.size() > 1 && base.back() == '/')
		base.pop_back();
	return base;
}

std::string api_base() {
	return normalize_api_base(OPT_GET("AI/OpenAI/Base URL")->GetString());
}

constexpr size_t proofread_max_input_chars = 900000;
constexpr size_t proofread_max_lines_per_request = 300;
#ifdef _WIN32
constexpr wchar_t credential_target[] = L"MutekiAegisub/AI/OpenAI/default";
constexpr wchar_t cloudinary_credential_target[] = L"MutekiAegisub/AI/Cloudinary/default";
#else
constexpr char credential_target[] = "OpenAI/default";
constexpr char cloudinary_credential_target[] = "Cloudinary/default";
#endif

std::mutex session_key_mutex;
std::string session_key;
std::mutex cloudinary_secret_mutex;
std::string session_cloudinary_secret;

size_t append_response(char *contents, size_t size, size_t nmemb, void *target) {
	static_cast<std::string *>(target)->append(contents, size * nmemb);
	return size * nmemb;
}

int progress_callback(void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto cancelled = static_cast<std::atomic_bool *>(data);
	return cancelled && cancelled->load() ? 1 : 0;
}

int function_progress_callback(void *data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
	auto callback = static_cast<std::function<bool()> *>(data);
	if (!callback || !*callback) return 0;
	try {
		return (*callback)() ? 1 : 0;
	}
	catch (...) {
		return 1;
	}
}

class CurlHandle final {
	CURL *handle = curl_easy_init();

public:
	CurlHandle(CurlHandle const&) = delete;
	CurlHandle& operator=(CurlHandle const&) = delete;
	CurlHandle() {
		if (!handle)
			throw Error("A libcurl inicializálása sikertelen.");
	}
	~CurlHandle() { curl_easy_cleanup(handle); }
	operator CURL *() const { return handle; }
};

class CurlHeaders final {
	curl_slist *headers = nullptr;

public:
	CurlHeaders() = default;
	CurlHeaders(CurlHeaders const&) = delete;
	CurlHeaders& operator=(CurlHeaders const&) = delete;
	CurlHeaders(CurlHeaders&& other) noexcept : headers(other.headers) {
		other.headers = nullptr;
	}
	CurlHeaders& operator=(CurlHeaders&& other) noexcept {
		if (this == &other) return *this;
		curl_slist_free_all(headers);
		headers = other.headers;
		other.headers = nullptr;
		return *this;
	}
	~CurlHeaders() { curl_slist_free_all(headers); }
	void Add(std::string const& value) {
		auto updated = curl_slist_append(headers, value.c_str());
		if (!updated)
			throw Error("A HTTP fejlécek létrehozása sikertelen.");
		headers = updated;
	}
	operator curl_slist *() const { return headers; }
};

void configure_common(CURL *curl, std::string const& api_key,
	std::string *response, std::atomic_bool *cancelled) {
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	// The API base URL is user-configurable, so keep transfers on HTTP(S)
	// instead of letting curl pick a protocol out of the configured address.
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "Aegisub-Muteki/AI");
	// Long transcription and review requests have no client-side time limit.
	// They remain cancellable through the transfer callback below.
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 0L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	// Never enable verbose curl logging here: it would expose the Authorization
	// header and therefore the user's API key.
	(void)api_key;
}

json::UnknownElement parse_json(std::string const& value) {
	std::istringstream stream(value);
	json::UnknownElement root;
	json::Reader::Read(root, stream);
	return root;
}

template<typename T>
std::string write_json(T const& value) {
	std::ostringstream stream;
	agi::JsonWriter::Write(value, stream);
	return stream.str();
}

std::string string_field(json::Object const& object, std::string_view name,
	std::string fallback = {}) {
	auto it = object.find(name);
	if (it == object.end()) return fallback;
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (json::Exception const&) {
		return fallback;
	}
}

std::string api_error(std::string const& response, long status) {
	try {
		auto root = parse_json(response);
		auto const& object = static_cast<json::Object const&>(root);
		auto it = object.find("error");
		if (it != object.end()) {
			auto const& error = static_cast<json::Object const&>(it->second);
			auto message = string_field(error, "message");
			if (!message.empty()) return message;
		}
	}
	catch (std::exception const&) {
	}
	return agi::format("Az OpenAI API HTTP %d hibával válaszolt.", status);
}

std::string perform(CURL *curl, CurlHeaders const& headers,
	std::atomic_bool *cancelled) {
	std::string response;
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, static_cast<curl_slist *>(headers));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	auto result = curl_easy_perform(curl);
	if (result == CURLE_ABORTED_BY_CALLBACK && cancelled && cancelled->load())
		throw Error("A kérés megszakítva.");
	if (result != CURLE_OK)
		throw Error(agi::format("Hálózati hiba: %s", curl_easy_strerror(result)));

	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	if (status < 200 || status >= 300)
		throw Error(api_error(response, status));
	return response;
}

CurlHeaders authenticated_headers(std::string const& key, bool json_body) {
	CurlHeaders headers;
	headers.Add("Authorization: Bearer " + key);
	if (json_body)
		headers.Add("Content-Type: application/json");
	return headers;
}

json::Object message_item(std::string const& role, std::string const& content) {
	json::Object message;
	message["role"] = role;
	message["content"] = content;
	return message;
}

json::Object type_schema(char const *type) {
	json::Object schema;
	schema["type"] = type;
	return schema;
}

json::Object line_schema() {
	json::Object properties;
	properties["line_id"] = type_schema("integer");
	properties["japanese"] = type_schema("string");
	properties["romaji"] = type_schema("string");
	json::Object verdict;
	verdict["type"] = "string";
	json::Array verdict_values;
	for (auto value : {"ok", "minor_issue", "major_issue"})
		verdict_values.emplace_back(value);
	verdict["enum"] = std::move(verdict_values);
	properties["verdict"] = std::move(verdict);
	properties["assessment"] = type_schema("string");
	properties["suggested_text"] = type_schema("string");

	json::Object issues;
	issues["type"] = "array";
	issues["items"] = type_schema("string");
	issues["maxItems"] = 3;
	properties["issues"] = std::move(issues);

	json::Array required;
	for (auto name : {"line_id", "japanese", "romaji", "verdict", "assessment", "issues", "suggested_text"})
		required.emplace_back(name);

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object response_schema() {
	json::Object line_array;
	line_array["type"] = "array";
	line_array["items"] = line_schema();

	json::Object properties;
	properties["message"] = type_schema("string");
	properties["lines"] = std::move(line_array);

	json::Array required;
	required.emplace_back("message");
	required.emplace_back("lines");

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object chat_response_schema() {
	json::Object properties;
	properties["message"] = type_schema("string");

	json::Array required;
	required.emplace_back("message");

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object proofread_issue_schema() {
	json::Object category;
	category["type"] = "string";
	json::Array category_values;
	for (auto value : {"spelling", "punctuation", "grammar", "style",
		"repetition", "consistency", "source_mismatch"})
		category_values.emplace_back(value);
	category["enum"] = std::move(category_values);

	json::Object categories;
	categories["type"] = "array";
	categories["items"] = std::move(category);

	json::Object suggestions;
	suggestions["type"] = "array";
	suggestions["items"] = type_schema("string");
	suggestions["maxItems"] = 3;

	json::Object properties;
	properties["line_id"] = type_schema("integer");
	properties["categories"] = std::move(categories);
	properties["explanation"] = type_schema("string");
	properties["suggestions"] = std::move(suggestions);

	json::Array required;
	for (auto name : {"line_id", "categories", "explanation", "suggestions"})
		required.emplace_back(name);

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object proofread_response_schema() {
	json::Object issues;
	issues["type"] = "array";
	issues["items"] = proofread_issue_schema();

	json::Object properties;
	properties["message"] = type_schema("string");
	properties["issues"] = std::move(issues);

	json::Array required;
	required.emplace_back("message");
	required.emplace_back("issues");

	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object karaoke_syllable_schema() {
	json::Object properties;
	properties["start_ms"] = type_schema("integer");
	properties["end_ms"] = type_schema("integer");
	properties["text"] = type_schema("string");
	properties["romaji"] = type_schema("string");
	json::Object language;
	language["type"] = "string";
	json::Array language_values;
	for (auto value : {"ja", "en", "other"}) language_values.emplace_back(value);
	language["enum"] = std::move(language_values);
	properties["language"] = std::move(language);
	json::Array required;
	for (auto name : {"start_ms", "end_ms", "text", "romaji", "language"})
		required.emplace_back(name);
	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object karaoke_response_schema() {
	json::Object syllables;
	syllables["type"] = "array";
	syllables["items"] = karaoke_syllable_schema();

	json::Object line_properties;
	line_properties["source_line_id"] = type_schema("integer");
	line_properties["start_ms"] = type_schema("integer");
	line_properties["end_ms"] = type_schema("integer");
	line_properties["romaji"] = type_schema("string");
	line_properties["syllables"] = std::move(syllables);
	json::Array line_required;
	for (auto name : {"source_line_id", "start_ms", "end_ms", "romaji", "syllables"})
		line_required.emplace_back(name);
	json::Object line;
	line["type"] = "object";
	line["properties"] = std::move(line_properties);
	line["required"] = std::move(line_required);
	line["additionalProperties"] = false;

	json::Object lines;
	lines["type"] = "array";
	lines["items"] = std::move(line);
	json::Object properties;
	properties["lines"] = std::move(lines);
	json::Array required;
	required.emplace_back("lines");
	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

json::Object kanji_response_schema() {
	json::Object line_properties;
	line_properties["source_line_id"] = type_schema("integer");
	line_properties["kanji"] = type_schema("string");
	json::Array line_required;
	line_required.emplace_back("source_line_id");
	line_required.emplace_back("kanji");
	json::Object line;
	line["type"] = "object";
	line["properties"] = std::move(line_properties);
	line["required"] = std::move(line_required);
	line["additionalProperties"] = false;

	json::Object lines;
	lines["type"] = "array";
	lines["items"] = std::move(line);
	json::Object properties;
	properties["lines"] = std::move(lines);
	json::Array required;
	required.emplace_back("lines");
	json::Object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = std::move(required);
	schema["additionalProperties"] = false;
	return schema;
}

std::string karaoke_instructions(KaraokeMode mode, bool advanced_timing) {
	std::string common =
		"You are a Japanese karaoke timing specialist. Use the supplied Japanese "
		"audio transcript and its measured time units as the timing authority. All "
		"times are integer milliseconds relative to the beginning of the supplied "
		"clip. Keep output chronological, inside the clip, and free of ASS tags. "
		"Split Japanese singing into individual mora-sized karaoke units, not words. "
		"A romaji unit is normally one Japanese mora and typically 1-3 Latin letters "
		"such as a, ka, shi, kyo, or n; never return a whole multi-mora word as one "
		"syllable. Split English singing into natural spoken English syllables, never "
		"individual letters. Set syllable.language to ja for Japanese, en for English, "
		"or other. Even when the singing is very fast, include every supplied lyric "
		"syllable and never omit text to make the timing easier. "
		"Anchor every syllable start and end to the actually sung phoneme. Do not spread "
		"syllables evenly across the subtitle line, and do not extend the final syllables "
		"into trailing silence. Preserve real silent gaps between sung syllables. Do not "
		"insert Japanese "
		"middle dots (・ or ･) or Latin middle dots (·). "
		"Every syllable must have start_ms < end_ms and syllable starts must increase. ";
	if (advanced_timing) {
		// Rules distilled from the Karaoke Mugen advanced timing guide. These stay
		// optional so users can compare them with the previous timing behaviour.
		common +=
			"Apply professional karaoke timing cues from the visible sound spectrum and "
			"the musical pulse. Prefer a nearby beat or a clear voiced onset over evenly "
			"distributed timing. For syllables beginning with s, z, x, soft c, f, h, or "
			"English th, exclude most high-frequency pre-beat fricative noise and place the "
			"boundary at the following beat or voiced onset. In initial st, sp, str, and "
			"similar clusters, normally time to the t or p burst; split a separately held s "
			"only when it is clearly sung as its own sound. For m, n, l, and y starts whose "
			"consonant onset is indistinct, use the beat or first vowel onset. Include the "
			"audible burst of k, t, tsu, chi, and similar plosives in their syllable. Preserve "
			"real breathing and silent gaps rather than stretching the preceding syllable. "
			"For Japanese, normally use one timing unit per kana or mora, keeping small "
			"ya/yu/yo compounds together. Use an apostrophe for an independent n before a "
			"vowel or y, as in ren'ai and kon'ya. Resolve doubled consonants, repeated vowels, "
			"long vowels, and final n from what is actually audible: never combine two "
			"distinct sung sound events into one karaoke unit, but do not force a split when "
			"the singer produces one continuous sound. ";
	}
	if (mode == KaraokeMode::AudioRecognition) {
		return common +
			"Return exactly one result for every supplied lyric line, in the given order, "
			"with its exact source_line_id, start_ms, and end_ms. Never omit, merge, split, "
			"add, or reorder lines. Recognize the singing inside each line's fixed time "
			"range and create accurate, easy-to-read Hepburn romaji. "
			"Split the romaji into natural, singable syllable groups and time them from the "
			"audio. In each syllable, text and romaji are the same romaji chunk. Return no "
			"kanji or translation.";
	}
	return common +
		"Return exactly one result for every supplied line and keep each line's given "
		"start_ms and end_ms. Set source_line_id to the exact input id. Split the exact "
		"input text among syllable.text without changing, omitting, or adding any byte; "
		"their concatenation must reproduce it exactly. Use the audio to time those "
		"chunks. Do not rewrite the line.";
}

std::string checked_language() {
	auto language = GetCheckLanguage();
	return language.empty() ? "Hungarian" : language;
}

std::string instructions(std::string const& custom) {
	std::string language = checked_language();
	std::string prompt =
		"You are a professional audiovisual subtitle quality reviewer. Audit the "
		"supplied existing " + language + " subtitle text; do not translate from scratch "
		"when the current subtitle is already correct. Compare current_text against "
		"the authoritative Japanese transcript and use source_reference as a secondary aid. "
		"Check meaning, omissions, additions, tone, natural " + language + " wording, grammar, "
		"and continuity across the scene. The input lines are ordered and belong to "
		"one continuous scene. Input lines are compact arrays in the exact order declared "
		"by columns. Return exactly one record for every supplied line_id "
		"in the same order. verdict must be ok, minor_issue, or major_issue. assessment "
		"must be one short " + language + " sentence and issues must contain at most three short "
		+ language + " items. suggested_text must be "
		"empty when no change is needed; otherwise it must contain one corrected, plain "
		+ language + " subtitle without ASS tags or literal newlines. japanese should contain "
		"the Japanese words assigned to the line, and romaji an easy-to-read Hepburn "
		"romanization. During follow-up chat, answer only the user's new question in "
		+ language + ". Do not repeat the per-line review unless explicitly asked.";
	if (!custom.empty())
		prompt += " Additional user review rules: " + custom;
	return prompt;
}

std::string proofread_instructions(std::string const& custom) {
	std::string language = checked_language();
	std::string prompt =
		"You are a meticulous " + language + " subtitle proofreader. Review only input lines "
		"where target is 1, and use every supplied line in the chronologically ordered "
		"batch as context. Return one combined issue object per flawed "
		"target line and no object for correct lines. Keep false positives very low for "
		"spelling claims. Check " + language + " spelling and typos, unambiguous comma and other "
		"punctuation errors, grammar, awkward or unclear wording in context, repeated words "
		"or phrases, and terminology/name/spelling consistency across the entire subtitle. "
		"Do not flag high CPS, reading speed, line length or subtitle duration issues. "
		"Treat source_line as a semantic reference: flag a contextually wrong but correctly "
		"spelled word of the target language when the source makes the "
		"mistake clear. Never propose a fresh translation merely because source_line differs. "
		"Editorial notes intentionally left in the subtitle are allowed. Never flag the presence, "
		"wording, formatting or retention of an editor/editorial note, and never suggest removing it. "
		"Input lines are compact arrays in the exact order declared by columns. Merge all findings "
		"for the same line into its categories, explanation and alternatives. explanation must be "
		"one short " + language + " sentence. Each suggestion is the complete "
		"replacement ASS text for that line, not merely a changed word. Preserve every existing "
		"ASS override block such as {\\i1} and {\\i0} verbatim in every suggestion and preserve "
		"explicit \\N, \\n and \\h controls unless reflow is necessary. Never add literal newlines. "
		"If categories is exactly [spelling] and the issue is only a clear typo, one strong "
		"suggestion is sufficient. For every other issue return exactly three genuinely useful, "
		"meaning-preserving alternatives, ordered best first. Do not return duplicate alternatives. "
		"If there are no findings, return an empty issues array and a short "
		+ language + " message.";
	if (!custom.empty())
		prompt += " Additional user review rules: " + custom;
	return prompt;
}

std::string extract_output_text(json::Object const& root) {
	auto output_it = root.find("output");
	if (output_it == root.end())
		throw Error("Az OpenAI válaszából hiányzik az output mező.");

	auto const& output = static_cast<json::Array const&>(output_it->second);
	for (auto const& item_value : output) {
		auto const& item = static_cast<json::Object const&>(item_value);
		if (string_field(item, "type") != "message") continue;
		auto content_it = item.find("content");
		if (content_it == item.end()) continue;
		for (auto const& content_value : static_cast<json::Array const&>(content_it->second)) {
			auto const& content = static_cast<json::Object const&>(content_value);
			if (string_field(content, "type") == "output_text") {
				auto text = string_field(content, "text");
				if (!text.empty()) return text;
			}
		}
	}
	throw Error("Az OpenAI nem adott szöveges választ.");
}

ReviewResult parse_review(std::string const& output_text,
	std::string conversation_json) {
	auto parsed = parse_json(output_text);
	auto const& object = static_cast<json::Object const&>(parsed);

	ReviewResult result;
	result.message = string_field(object, "message");
	result.conversation_json = std::move(conversation_json);

	auto lines_it = object.find("lines");
	if (lines_it == object.end())
		throw Error("Az ellenőrzési válaszból hiányzik a lines mező.");

	for (auto const& value : static_cast<json::Array const&>(lines_it->second)) {
		auto const& line = static_cast<json::Object const&>(value);
		LineReview review;
		review.id = static_cast<int>(static_cast<json::Integer const&>(line.at("line_id")));
		review.japanese = string_field(line, "japanese");
		review.romaji = string_field(line, "romaji");
		review.verdict = string_field(line, "verdict");
		review.assessment = string_field(line, "assessment");
		review.suggested_text = string_field(line, "suggested_text");
		auto issues_it = line.find("issues");
		if (issues_it != line.end()) {
			for (auto const& issue : static_cast<json::Array const&>(issues_it->second))
				review.issues.push_back(static_cast<json::String const&>(issue));
		}
		result.lines.push_back(std::move(review));
	}
	return result;
}

ReviewResult parse_chat(std::string const& output_text,
	std::string conversation_json) {
	auto parsed = parse_json(output_text);
	auto const& object = static_cast<json::Object const&>(parsed);
	ReviewResult result;
	result.message = string_field(object, "message");
	result.conversation_json = std::move(conversation_json);
	if (result.message.empty())
		throw Error("Az AI üres chatválaszt adott.");
	return result;
}

ProofreadResult parse_proofread(std::string const& output_text) {
	auto parsed = parse_json(output_text);
	auto const& object = static_cast<json::Object const&>(parsed);
	ProofreadResult result;
	result.message = string_field(object, "message");

	auto issues_it = object.find("issues");
	if (issues_it == object.end())
		throw Error("Az utóellenőrzési válaszból hiányzik az issues mező.");

	for (auto const& value : static_cast<json::Array const&>(issues_it->second)) {
		auto const& object = static_cast<json::Object const&>(value);
		ProofreadIssue issue;
		issue.line_id = static_cast<int>(static_cast<json::Integer const&>(object.at("line_id")));
		issue.explanation = string_field(object, "explanation");
		for (auto const& category : static_cast<json::Array const&>(object.at("categories")))
			issue.categories.push_back(static_cast<json::String const&>(category));
		for (auto const& suggestion : static_cast<json::Array const&>(object.at("suggestions"))) {
			auto text = static_cast<json::String const&>(suggestion);
			if (std::find(issue.suggestions.begin(), issue.suggestions.end(), text) == issue.suggestions.end())
				issue.suggestions.push_back(std::move(text));
		}

		// A partially useful result is preferable to discarding a long whole-file
		// review. The prompt requests three alternatives where appropriate, but a
		// short model response is still safe to present to the user.
		if (issue.suggestions.empty()) continue;
		result.issues.push_back(std::move(issue));
	}
	return result;
}

KaraokeResult parse_karaoke(std::string const& output_text) {
	auto parsed = parse_json(output_text);
	auto const& object = static_cast<json::Object const&>(parsed);
	auto lines_it = object.find("lines");
	if (lines_it == object.end())
		throw Error("A karaoke-válaszból hiányzik a lines mező.");

	KaraokeResult result;
	for (auto const& value : static_cast<json::Array const&>(lines_it->second)) {
		auto const& item = static_cast<json::Object const&>(value);
		KaraokeLine line;
		line.source_line_id = static_cast<int>(static_cast<json::Integer const&>(item.at("source_line_id")));
		line.start_ms = static_cast<int>(static_cast<json::Integer const&>(item.at("start_ms")));
		line.end_ms = static_cast<int>(static_cast<json::Integer const&>(item.at("end_ms")));
		line.kanji = string_field(item, "kanji");
		line.romaji = string_field(item, "romaji");
		for (auto const& syllable_value : static_cast<json::Array const&>(item.at("syllables"))) {
			auto const& syllable_item = static_cast<json::Object const&>(syllable_value);
			KaraokeSyllable syllable;
			syllable.start_ms = static_cast<int>(static_cast<json::Integer const&>(syllable_item.at("start_ms")));
			syllable.end_ms = static_cast<int>(static_cast<json::Integer const&>(syllable_item.at("end_ms")));
			syllable.text = string_field(syllable_item, "text");
			syllable.kanji = string_field(syllable_item, "kanji");
			syllable.romaji = string_field(syllable_item, "romaji");
			syllable.language = string_field(syllable_item, "language");
			line.syllables.push_back(std::move(syllable));
		}
		result.lines.push_back(std::move(line));
	}
	return result;
}

KaraokeResult parse_kanji(std::string const& output_text) {
	auto parsed = parse_json(output_text);
	auto const& object = static_cast<json::Object const&>(parsed);
	auto lines_it = object.find("lines");
	if (lines_it == object.end())
		throw Error("A kanji-válaszból hiányzik a lines mező.");

	KaraokeResult result;
	for (auto const& value : static_cast<json::Array const&>(lines_it->second)) {
		auto const& item = static_cast<json::Object const&>(value);
		KaraokeLine line;
		line.source_line_id = static_cast<int>(static_cast<json::Integer const&>(item.at("source_line_id")));
		line.kanji = string_field(item, "kanji");
		result.lines.push_back(std::move(line));
	}
	return result;
}

std::string append_response_items(std::string const& history_json,
	json::Object& response_root) {
	auto history_root = parse_json(history_json);
	auto history = std::move(static_cast<json::Array&>(history_root));
	auto output_it = response_root.find("output");
	if (output_it == response_root.end())
		throw Error("Az OpenAI válaszából hiányzik az output mező.");
	auto& output = static_cast<json::Array&>(output_it->second);
	for (auto& item : output)
		history.push_back(std::move(item));
	return write_json(history);
}

std::string post_json(std::string const& key, std::string const& endpoint,
	std::string const& body, std::atomic_bool *cancelled) {
	CurlHandle curl;
	std::string response;
	configure_common(curl, key, &response, cancelled);
	curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
	auto headers = authenticated_headers(key, true);
	return perform(curl, headers, cancelled);
}

ReviewResult structured_request(std::string const& key,
	std::string const& model, std::string const& custom_instructions,
	std::string history_json, std::string const& user_message,
	std::atomic_bool *cancelled, bool include_line_reviews) {
	auto history_root = parse_json(history_json);
	auto history = std::move(static_cast<json::Array&>(history_root));
	history.emplace_back(message_item("user", user_message));
	auto updated_history_json = write_json(history);

	// Parse the just-serialized history again because Cajun values are move-only
	// and the request takes ownership of the input array.
	auto request_history_root = parse_json(updated_history_json);
	auto request_history = std::move(static_cast<json::Array&>(request_history_root));

	json::Object format;
	format["type"] = "json_schema";
	format["name"] = include_line_reviews ? "subtitle_review" : "subtitle_review_chat";
	format["strict"] = true;
	format["schema"] = include_line_reviews ? response_schema() : chat_response_schema();

	json::Object text;
	text["format"] = std::move(format);
	text["verbosity"] = include_line_reviews ? "low" : "medium";

	json::Object reasoning;
	reasoning["effort"] = "low";

	json::Object request;
	request["model"] = model;
	request["instructions"] = instructions(custom_instructions);
	request["input"] = std::move(request_history);
	request["text"] = std::move(text);
	request["reasoning"] = std::move(reasoning);
	request["store"] = false;
	request["max_output_tokens"] = include_line_reviews ? 12000 : 4000;

	auto response_text = post_json(key, api_base() + "/responses",
		write_json(request), cancelled);
	auto response_root_value = parse_json(response_text);
	auto& response_root = static_cast<json::Object&>(response_root_value);
	auto output_text = extract_output_text(response_root);
	auto complete_history = append_response_items(updated_history_json, response_root);
	return include_line_reviews
		? parse_review(output_text, std::move(complete_history))
		: parse_chat(output_text, std::move(complete_history));
}

ProofreadResult proofread_request(std::string const& key,
	std::string const& model, std::string const& custom_instructions,
	std::string const& user_message, std::atomic_bool *cancelled) {
	json::Array input;
	input.emplace_back(message_item("user", user_message));

	json::Object format;
	format["type"] = "json_schema";
	format["name"] = "subtitle_proofread";
	format["strict"] = true;
	format["schema"] = proofread_response_schema();

	json::Object text;
	text["format"] = std::move(format);
	text["verbosity"] = "low";

	json::Object reasoning;
	reasoning["effort"] = "low";

	json::Object request;
	request["model"] = model;
	request["instructions"] = proofread_instructions(custom_instructions);
	request["input"] = std::move(input);
	request["text"] = std::move(text);
	request["reasoning"] = std::move(reasoning);
	request["store"] = false;
	request["max_output_tokens"] = 16000;

	auto response_text = post_json(key, api_base() + "/responses",
		write_json(request), cancelled);
	auto response_root_value = parse_json(response_text);
	auto const& response_root = static_cast<json::Object const&>(response_root_value);
	return parse_proofread(extract_output_text(response_root));
}

std::string read_environment_key() {
	auto value = std::getenv("OPENAI_API_KEY");
	return value ? value : "";
}

std::string read_environment_cloudinary_secret() {
	auto value = std::getenv("CLOUDINARY_API_SECRET");
	return value ? value : "";
}

#ifdef _WIN32
std::string read_credential_key() {
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(credential_target, CRED_TYPE_GENERIC, 0, &credential))
		return {};
	std::string value(reinterpret_cast<char const *>(credential->CredentialBlob),
		credential->CredentialBlobSize);
	CredFree(credential);
	return value;
}

std::string read_cloudinary_credential_secret() {
	PCREDENTIALW credential = nullptr;
	if (!CredReadW(cloudinary_credential_target, CRED_TYPE_GENERIC, 0, &credential))
		return {};
	std::string value(reinterpret_cast<char const *>(credential->CredentialBlob),
		credential->CredentialBlobSize);
	CredFree(credential);
	return value;
}

std::string windows_error_message(DWORD code) {
	wchar_t *buffer = nullptr;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
		FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
		reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
	std::wstring wide = buffer ? buffer : L"Ismeretlen Windows hiba";
	if (buffer) LocalFree(buffer);
	return std::string(wide.begin(), wide.end());
}
#elif defined(__APPLE__)
constexpr char credential_service[] = "MutekiAegisub/AI";

std::string macos_error_message(OSStatus status) {
	CFStringRef message = SecCopyErrorMessageString(status, nullptr);
	if (!message) return agi::format("macOS Keychain error: %d", status);
	CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(message),
		kCFStringEncodingUTF8) + 1;
	std::vector<char> buffer(static_cast<size_t>(size));
	bool converted = CFStringGetCString(message, buffer.data(), size, kCFStringEncodingUTF8);
	CFRelease(message);
	return converted ? std::string(buffer.data()) : agi::format("macOS Keychain error: %d", status);
}

std::string read_keychain_secret(char const *account) {
	UInt32 length = 0;
	void *data = nullptr;
	OSStatus status = SecKeychainFindGenericPassword(nullptr,
		static_cast<UInt32>(std::strlen(credential_service)), credential_service,
		static_cast<UInt32>(std::strlen(account)), account, &length, &data, nullptr);
	if (status != errSecSuccess) return {};
	std::string value(static_cast<char const *>(data), length);
	SecKeychainItemFreeContent(nullptr, data);
	return value;
}

bool store_keychain_secret(char const *account, std::string const& value, std::string *error) {
	SecKeychainItemRef item = nullptr;
	OSStatus status = SecKeychainFindGenericPassword(nullptr,
		static_cast<UInt32>(std::strlen(credential_service)), credential_service,
		static_cast<UInt32>(std::strlen(account)), account, nullptr, nullptr, &item);
	if (status == errSecSuccess) {
		status = SecKeychainItemModifyAttributesAndData(item, nullptr,
			static_cast<UInt32>(value.size()), value.data());
		CFRelease(item);
	}
	else if (status == errSecItemNotFound) {
		status = SecKeychainAddGenericPassword(nullptr,
			static_cast<UInt32>(std::strlen(credential_service)), credential_service,
			static_cast<UInt32>(std::strlen(account)), account,
			static_cast<UInt32>(value.size()), value.data(), nullptr);
	}
	if (status == errSecSuccess) return true;
	if (error) *error = macos_error_message(status);
	return false;
}

bool delete_keychain_secret(char const *account, std::string *error) {
	SecKeychainItemRef item = nullptr;
	OSStatus status = SecKeychainFindGenericPassword(nullptr,
		static_cast<UInt32>(std::strlen(credential_service)), credential_service,
		static_cast<UInt32>(std::strlen(account)), account, nullptr, nullptr, &item);
	if (status == errSecItemNotFound) return true;
	if (status == errSecSuccess) {
		status = SecKeychainItemDelete(item);
		CFRelease(item);
	}
	if (status == errSecSuccess) return true;
	if (error) *error = macos_error_message(status);
	return false;
}
#elif defined(WITH_LIBSECRET)
SecretSchema const credential_schema = {
	"com.muteki.Aegisub", SECRET_SCHEMA_NONE,
	{{"account", SECRET_SCHEMA_ATTRIBUTE_STRING}, {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}
};

std::string read_secret_service_secret(char const *account) {
	GError *error = nullptr;
	char *value = secret_password_lookup_sync(&credential_schema, nullptr, &error,
		"account", account, nullptr);
	if (error) g_error_free(error);
	if (!value) return {};
	std::string result(value);
	secret_password_free(value);
	return result;
}

bool store_secret_service_secret(char const *account, char const *label,
	std::string const& value, std::string *message) {
	GError *error = nullptr;
	gboolean stored = secret_password_store_sync(&credential_schema,
		SECRET_COLLECTION_DEFAULT, label, value.c_str(), nullptr, &error,
		"account", account, nullptr);
	if (stored) return true;
	if (message) *message = error ? error->message : "Secret Service could not store the credential.";
	if (error) g_error_free(error);
	return false;
}

bool delete_secret_service_secret(char const *account, std::string *message) {
	GError *error = nullptr;
	gboolean removed = secret_password_clear_sync(&credential_schema, nullptr, &error,
		"account", account, nullptr);
	if (removed) return true;
	if (message) *message = error ? error->message : "Secret Service could not delete the credential.";
	if (error) g_error_free(error);
	return false;
}
#endif

} // namespace

std::string DefaultApiBase() {
	return default_api_base;
}

OpenAIClient::OpenAIClient(std::string api_key, std::string model,
	std::string transcription_model, std::string custom_instructions,
	std::atomic_bool *cancelled)
: api_key(std::move(api_key))
, model(std::move(model))
, transcription_model(std::move(transcription_model))
, custom_instructions(std::move(custom_instructions))
, cancelled(cancelled) {
	if (this->api_key.empty()) throw Error("Nincs beállítva OpenAI API-kulcs.");
	if (this->model.empty()) throw Error("Nincs beállítva ellenőrzési modell.");
	if (this->transcription_model.empty()) throw Error("Nincs beállítva beszédfelismerési modell.");
}

void OpenAIClient::TestConnection(std::string const& base_url) const {
	auto const base = normalize_api_base(base_url);
	CurlHandle curl;
	std::string response;
	configure_common(curl, api_key, &response, cancelled);
	char *escaped = curl_easy_escape(curl, model.c_str(), static_cast<int>(model.size()));
	if (!escaped) throw Error("A modellnév kódolása sikertelen.");
	std::string url = base + "/models/" + escaped;
	curl_free(escaped);
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	auto headers = authenticated_headers(api_key, false);
	(void)perform(curl, headers, cancelled);
}

std::string OpenAIClient::Transcribe(agi::fs::path const& audio_file) const {
	CurlHandle curl;
	std::string response;
	configure_common(curl, api_key, &response, cancelled);
	curl_easy_setopt(curl, CURLOPT_URL, (api_base() + "/audio/transcriptions").c_str());

	curl_mime *mime = curl_mime_init(curl);
	if (!mime) throw Error("A hangfeltöltés előkészítése sikertelen.");
	auto cleanup = [&] { curl_mime_free(mime); };

	auto add_text = [&](char const *name, std::string const& value) {
		auto part = curl_mime_addpart(mime);
		curl_mime_name(part, name);
		curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
	};

	add_text("model", transcription_model);
	add_text("response_format", "json");
	add_text("language", "ja");
	auto file_part = curl_mime_addpart(mime);
	curl_mime_name(file_part, "file");
	curl_mime_filedata(file_part, audio_file.string().c_str());
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

	try {
		auto headers = authenticated_headers(api_key, false);
		response = perform(curl, headers, cancelled);
		cleanup();
	}
	catch (...) {
		cleanup();
		throw;
	}

	try {
		auto parsed = parse_json(response);
		auto const& root = static_cast<json::Object const&>(parsed);
		auto text = string_field(root, "text");
		if (text.empty()) throw Error("A beszédfelismerés üres eredményt adott.");
		return text;
	}
	catch (Error const&) { throw; }
	catch (std::exception const& error) {
		throw Error(agi::format("A beszédfelismerési válasz nem értelmezhető: %s", error.what()));
	}
}

std::string EditImage(std::string const& api_key, std::string const& image_model,
	std::vector<unsigned char> const& image_png,
	std::vector<unsigned char> const& mask_png, std::string const& size,
	std::string const& prompt, std::function<bool()> const& is_cancelled) {
	if (api_key.empty()) throw Error("Nincs beállítva OpenAI API-kulcs.");
	if (image_model.empty()) throw Error("Nincs beállítva képgenerálási modell.");
	if (image_png.empty() || mask_png.empty()) throw Error("A jelenet vagy a maszk képe üres.");

	CurlHandle curl;
	std::string response;
	std::function<bool()> cancel_check = is_cancelled;
	configure_common(curl, api_key, &response, nullptr);
	curl_easy_setopt(curl, CURLOPT_URL, (api_base() + "/images/edits").c_str());

	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, function_progress_callback);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancel_check);

	curl_mime *mime = curl_mime_init(curl);
	if (!mime) throw Error("A képfeltöltés előkészítése sikertelen.");
	auto cleanup = [&] { curl_mime_free(mime); };
	auto add_text = [&](char const *name, std::string const& value) {
		auto part = curl_mime_addpart(mime);
		curl_mime_name(part, name);
		curl_mime_data(part, value.data(), value.size());
	};
	auto add_png = [&](char const *name, char const *filename,
		std::vector<unsigned char> const& data) {
		auto part = curl_mime_addpart(mime);
		curl_mime_name(part, name);
		curl_mime_filename(part, filename);
		curl_mime_type(part, "image/png");
		curl_mime_data(part, reinterpret_cast<char const *>(data.data()), data.size());
	};

	add_text("model", image_model);
	add_text("prompt", prompt);
	add_text("size", size);
	add_text("quality", "high");
	add_text("output_format", "png");
	add_png("image[]", "scene.png", image_png);
	add_png("mask", "mask.png", mask_png);
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

	try {
		auto headers = authenticated_headers(api_key, false);
		response = perform(curl, headers, nullptr);
		cleanup();
	}
	catch (...) {
		cleanup();
		if (cancel_check && cancel_check())
			throw Error("A kérés megszakítva.");
		throw;
	}

	try {
		auto parsed = parse_json(response);
		auto const& root = static_cast<json::Object const&>(parsed);
		auto data_it = root.find("data");
		if (data_it == root.end()) throw Error("A képgenerálási válaszból hiányzik az eredmény.");
		auto const& data = static_cast<json::Array const&>(data_it->second);
		if (data.empty()) throw Error("A képgenerálás üres eredményt adott.");
		auto const& first = static_cast<json::Object const&>(*data.begin());
		auto encoded = string_field(first, "b64_json");
		if (encoded.empty()) throw Error("A képgenerálás nem adott vissza PNG-képet.");
		return encoded;
	}
	catch (Error const&) { throw; }
	catch (std::exception const& error) {
		throw Error(agi::format("A képgenerálási válasz nem értelmezhető: %s", error.what()));
	}
}

TimedTranscript OpenAIClient::TranscribeTimed(agi::fs::path const& audio_file) const {
	CurlHandle curl;
	std::string response;
	configure_common(curl, api_key, &response, cancelled);
	curl_easy_setopt(curl, CURLOPT_URL, (api_base() + "/audio/transcriptions").c_str());

	curl_mime *mime = curl_mime_init(curl);
	if (!mime) throw Error("A hangfeltöltés előkészítése sikertelen.");
	auto cleanup = [&] { curl_mime_free(mime); };
	auto add_text = [&](char const *name, std::string const& value) {
		auto part = curl_mime_addpart(mime);
		curl_mime_name(part, name);
		curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
	};
	add_text("model", transcription_model);
	add_text("response_format", "verbose_json");
	add_text("timestamp_granularities[]", "word");
	add_text("timestamp_granularities[]", "segment");
	add_text("language", "ja");
	auto file_part = curl_mime_addpart(mime);
	curl_mime_name(file_part, "file");
	curl_mime_filedata(file_part, audio_file.string().c_str());
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	try {
		auto headers = authenticated_headers(api_key, false);
		response = perform(curl, headers, cancelled);
		cleanup();
	}
	catch (...) {
		cleanup();
		throw;
	}

	auto number_field = [](json::Object const& object, char const *name) {
		auto const& value = object.at(name);
		try { return static_cast<double>(static_cast<json::Double const&>(value)); }
		catch (json::Exception const&) {
			return static_cast<double>(static_cast<json::Integer const&>(value));
		}
	};
	try {
		auto parsed = parse_json(response);
		auto const& root = static_cast<json::Object const&>(parsed);
		TimedTranscript transcript;
		transcript.text = string_field(root, "text");
		auto append_units = [&](char const *field) {
			auto it = root.find(field);
			if (it == root.end()) return;
			for (auto const& value : static_cast<json::Array const&>(it->second)) {
				auto const& unit = static_cast<json::Object const&>(value);
				auto text = string_field(unit, "word", string_field(unit, "text"));
				if (text.empty()) continue;
				transcript.units.push_back({
					static_cast<int>(std::lround(number_field(unit, "start") * 1000.0)),
					static_cast<int>(std::lround(number_field(unit, "end") * 1000.0)),
					std::move(text)});
			}
		};
		append_units("words");
		if (transcript.units.empty()) append_units("segments");
		if (transcript.text.empty() || transcript.units.empty())
			throw Error("A beszédfelismerés nem adott időbélyeges japán átiratot. Ellenőrizd, hogy a beállított átírási modell támogatja-e a verbose_json időbélyegeket.");
		return transcript;
	}
	catch (Error const&) { throw; }
	catch (std::exception const& error) {
		throw Error(agi::format("Az időbélyeges beszédfelismerési válasz nem értelmezhető: %s", error.what()));
	}
}

KaraokeResult OpenAIClient::CreateKaraoke(KaraokeMode mode,
	std::vector<KaraokeInputLine> const& lines,
	TimedTranscript const& transcript, bool advanced_timing) const {
	int context_start = lines.empty() ? 0 : lines.front().start_ms;
	int context_end = lines.empty() ? 0 : lines.front().end_ms;
	for (auto const& line : lines) {
		context_start = std::min(context_start, line.start_ms);
		context_end = std::max(context_end, line.end_ms);
	}
	context_start = std::max(0, context_start - 750);
	context_end += 750;
	json::Array transcript_units;
	std::string relevant_transcript;
	for (auto const& unit : transcript.units) {
		if (!lines.empty() && (unit.end_ms < context_start || unit.start_ms > context_end)) continue;
		json::Array row;
		row.emplace_back(unit.start_ms);
		row.emplace_back(unit.end_ms);
		row.emplace_back(unit.text);
		transcript_units.emplace_back(std::move(row));
		if (!relevant_transcript.empty()) relevant_transcript += ' ';
		relevant_transcript += unit.text;
	}
	if (transcript_units.empty()) {
		for (auto const& unit : transcript.units) {
			json::Array row;
			row.emplace_back(unit.start_ms);
			row.emplace_back(unit.end_ms);
			row.emplace_back(unit.text);
			transcript_units.emplace_back(std::move(row));
		}
		relevant_transcript = transcript.text;
	}
	json::Array input_lines;
	for (auto const& line : lines) {
		json::Array row;
		row.emplace_back(line.id);
		row.emplace_back(line.start_ms);
		row.emplace_back(line.end_ms);
		row.emplace_back(line.text);
		input_lines.emplace_back(std::move(row));
	}
	json::Object context;
	context["mode"] = mode == KaraokeMode::AudioRecognition ? "audio_recognition" : "syllable_timing";
	context["advanced_timing"] = advanced_timing;
	context["transcript"] = relevant_transcript;
	json::Array transcript_columns;
	for (auto name : {"start_ms", "end_ms", "text"}) transcript_columns.emplace_back(name);
	context["transcript_columns"] = std::move(transcript_columns);
	context["transcript_units"] = std::move(transcript_units);
	json::Array line_columns;
	for (auto name : {"line_id", "start_ms", "end_ms", "text"}) line_columns.emplace_back(name);
	context["line_columns"] = std::move(line_columns);
	context["lines"] = std::move(input_lines);

	json::Array input;
	input.emplace_back(message_item("user", write_json(context)));
	json::Object format;
	format["type"] = "json_schema";
	format["name"] = "aegisub_karaoke";
	format["strict"] = true;
	format["schema"] = karaoke_response_schema();
	json::Object text;
	text["format"] = std::move(format);
	text["verbosity"] = "low";
	json::Object reasoning;
	reasoning["effort"] = mode == KaraokeMode::AudioRecognition ? "low" : "high";
	json::Object request;
	request["model"] = model;
	request["instructions"] = karaoke_instructions(mode, advanced_timing);
	request["input"] = std::move(input);
	request["text"] = std::move(text);
	request["reasoning"] = std::move(reasoning);
	request["store"] = false;
	request["max_output_tokens"] = 16000;
	auto response_text = post_json(api_key, api_base() + "/responses",
		write_json(request), cancelled);
	auto response_root_value = parse_json(response_text);
	auto const& response_root = static_cast<json::Object const&>(response_root_value);
	return parse_karaoke(extract_output_text(response_root));
}

KaraokeResult OpenAIClient::CreateKanji(std::vector<KaraokeInputLine> const& lines) const {
	json::Array input_lines;
	for (auto const& line : lines) {
		json::Array row;
		row.emplace_back(line.id);
		row.emplace_back(line.text);
		input_lines.emplace_back(std::move(row));
	}
	json::Object context;
	context["task"] = "romaji_to_japanese_lyrics";
	json::Array columns;
	columns.emplace_back("line_id");
	columns.emplace_back("romaji");
	context["columns"] = std::move(columns);
	context["lines"] = std::move(input_lines);

	json::Array input;
	input.emplace_back(message_item("user", write_json(context)));
	json::Object format;
	format["type"] = "json_schema";
	format["name"] = "aegisub_kanji_lines";
	format["strict"] = true;
	format["schema"] = kanji_response_schema();
	json::Object text;
	text["format"] = std::move(format);
	text["verbosity"] = "low";
	json::Object reasoning;
	reasoning["effort"] = "low";
	json::Object request;
	request["model"] = model;
	request["instructions"] =
		"Convert Japanese song lyrics written in romaji to accurate natural Japanese "
		"kanji and kana. Return exactly one result for every input line, in the same "
		"order and with its exact source_line_id. Preserve the meaning, wording, names, "
		"repetitions, and line boundaries. Return plain subtitle text without ASS tags, "
		"karaoke timing, translations, or middle dots (・, ･, or ·). If a line is entirely "
		"English rather than Japanese romaji, return an empty kanji string for that line.";
	request["input"] = std::move(input);
	request["text"] = std::move(text);
	request["reasoning"] = std::move(reasoning);
	request["store"] = false;
	request["max_output_tokens"] = 8000;
	auto response_text = post_json(api_key, api_base() + "/responses",
		write_json(request), cancelled);
	auto response_root_value = parse_json(response_text);
	auto const& response_root = static_cast<json::Object const&>(response_root_value);
	return parse_kanji(extract_output_text(response_root));
}

ReviewResult OpenAIClient::Review(std::vector<SubtitleLine> const& lines,
	std::string const& japanese_transcript) const {
	json::Array input_lines;
	for (auto const& line : lines) {
		json::Array row;
		row.emplace_back(line.id);
		row.emplace_back(line.source_text);
		row.emplace_back(line.current_text);
		row.emplace_back(line.actor);
		input_lines.emplace_back(std::move(row));
	}

	json::Object context;
	context["japanese_transcript"] = japanese_transcript;
	context["review_language"] = checked_language();
	context["task"] = "quality_review_existing_subtitles";
	json::Array columns;
	for (auto name : {"line_id", "source_reference", "current_text", "actor"})
		columns.emplace_back(name);
	context["columns"] = std::move(columns);
	context["lines"] = std::move(input_lines);
	return structured_request(api_key, model, custom_instructions, "[]",
		write_json(context), cancelled, true);
}

ReviewResult OpenAIClient::Continue(ReviewResult const& previous,
	std::string const& user_message) const {
	if (user_message.empty()) throw Error("A chatüzenet nem lehet üres.");
	auto result = structured_request(api_key, model, custom_instructions,
		previous.conversation_json, user_message, cancelled, false);
	result.lines = previous.lines;
	return result;
}

ProofreadResult OpenAIClient::Proofread(std::vector<SubtitleLine> const& lines) const {
	auto make_context = [](std::vector<SubtitleLine const *> const& batch) {
		json::Array input_lines;
		for (auto line : batch) {
			json::Array row;
			row.emplace_back(line->id);
			row.emplace_back(line->target ? 1 : 0);
			row.emplace_back(line->source_text);
			row.emplace_back(line->ass_text);
			row.emplace_back(line->actor);
			input_lines.emplace_back(std::move(row));
		}

		json::Object context;
		context["language"] = checked_language();
		context["task"] = "subtitle_proofread";
		json::Array columns;
		for (auto name : {"line_id", "target", "source_line", "ass_text", "actor"})
			columns.emplace_back(name);
		context["columns"] = std::move(columns);
		context["lines"] = std::move(input_lines);
		return write_json(context);
	};

	ProofreadResult combined;
	std::vector<SubtitleLine const *> batch;
	size_t estimated_chars = 256;
	size_t request_count = 0;
	auto send_batch = [&] {
		if (batch.empty()) return;
		auto message = make_context(batch);
		if (message.size() > proofread_max_input_chars)
			throw Error("Az AI-utóellenőrzés egyik feliratsora túl hosszú a feldolgozáshoz.");
		auto part = proofread_request(api_key, model, custom_instructions,
			message, cancelled);
		if (request_count++ == 0)
			combined.message = std::move(part.message);
		combined.issues.insert(combined.issues.end(),
			std::make_move_iterator(part.issues.begin()),
			std::make_move_iterator(part.issues.end()));
		batch.clear();
		estimated_chars = 256;
	};

	for (auto const& line : lines) {
		json::Array row;
		row.emplace_back(line.id);
		row.emplace_back(line.target ? 1 : 0);
		row.emplace_back(line.source_text);
		row.emplace_back(line.ass_text);
		row.emplace_back(line.actor);
		auto row_chars = write_json(row).size() + 1;
		if (row_chars + 256 > proofread_max_input_chars)
			throw Error("Az AI-utóellenőrzés egyik feliratsora túl hosszú a feldolgozáshoz.");
		if (!batch.empty() && (batch.size() >= proofread_max_lines_per_request ||
			estimated_chars + row_chars > proofread_max_input_chars))
			send_batch();
		batch.push_back(&line);
		estimated_chars += row_chars;
	}
	send_batch();
	if (request_count > 1)
		combined.message.clear();
	return combined;
}

std::string GetCheckLanguage() {
	return OPT_GET("AI/Check Language")->GetString();
}

void SetCheckLanguage(std::string language) {
	OPT_SET("AI/Check Language")->SetString(std::move(language));
}

std::vector<std::string> CheckLanguageChoices() {
	// English names on purpose: the review and proofread prompts name the language
	// to the model, and an English name is what it reliably understands.
	return {"Arabic", "Bulgarian", "Chinese (Simplified)", "Chinese (Traditional)",
		"Croatian", "Czech", "Danish", "Dutch", "English", "Estonian", "Finnish",
		"French", "German", "Greek", "Hebrew", "Hindi", "Hungarian", "Indonesian",
		"Italian", "Japanese", "Korean", "Latvian", "Lithuanian", "Norwegian",
		"Polish", "Portuguese", "Portuguese (Brazilian)", "Romanian", "Russian",
		"Serbian", "Slovak", "Slovenian", "Spanish", "Spanish (Latin American)",
		"Swedish", "Thai", "Turkish", "Ukrainian", "Vietnamese"};
}

std::string GetApiKey() {
	{
		std::lock_guard<std::mutex> lock(session_key_mutex);
		if (!session_key.empty()) return session_key;
	}
	auto environment = read_environment_key();
	if (!environment.empty()) return environment;
#ifdef _WIN32
	return read_credential_key();
#elif defined(__APPLE__)
	return read_keychain_secret(credential_target);
#elif defined(WITH_LIBSECRET)
	return read_secret_service_secret(credential_target);
#else
	return {};
#endif
}

ApiKeySource GetApiKeySource() {
	{
		std::lock_guard<std::mutex> lock(session_key_mutex);
		if (!session_key.empty()) return ApiKeySource::Session;
	}
	if (!read_environment_key().empty()) return ApiKeySource::Environment;
#ifdef _WIN32
	if (!read_credential_key().empty()) return ApiKeySource::CredentialManager;
#elif defined(__APPLE__)
	if (!read_keychain_secret(credential_target).empty()) return ApiKeySource::CredentialManager;
#elif defined(WITH_LIBSECRET)
	if (!read_secret_service_secret(credential_target).empty()) return ApiKeySource::CredentialManager;
#endif
	return ApiKeySource::None;
}

void SetSessionApiKey(std::string key) {
	std::lock_guard<std::mutex> lock(session_key_mutex);
	session_key = std::move(key);
}

void ClearSessionApiKey() {
	std::lock_guard<std::mutex> lock(session_key_mutex);
	std::fill(session_key.begin(), session_key.end(), '\0');
	session_key.clear();
}

bool StoreApiKey(std::string const& key, std::string *error) {
	if (key.empty()) {
		if (error) *error = "Az API-kulcs nem lehet üres.";
		return false;
	}
#ifdef _WIN32
	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = const_cast<wchar_t *>(credential_target);
	credential.CredentialBlobSize = static_cast<DWORD>(key.size());
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(key.data()));
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	credential.UserName = const_cast<wchar_t *>(L"OpenAI API");
	if (!CredWriteW(&credential, 0)) {
		if (error) *error = windows_error_message(GetLastError());
		return false;
	}
	SetSessionApiKey(key);
	return true;
#elif defined(__APPLE__)
	if (!store_keychain_secret(credential_target, key, error)) return false;
	SetSessionApiKey(key);
	return true;
#elif defined(WITH_LIBSECRET)
	if (!store_secret_service_secret(credential_target, "Muteki Aegisub OpenAI API key",
		key, error)) return false;
	SetSessionApiKey(key);
	return true;
#else
	if (error) *error = "A biztonságos mentés ezen a platformon még nem támogatott; használja az OPENAI_API_KEY környezeti változót.";
	return false;
#endif
}

bool DeleteStoredApiKey(std::string *error) {
	ClearSessionApiKey();
#ifdef _WIN32
	if (CredDeleteW(credential_target, CRED_TYPE_GENERIC, 0)) return true;
	auto code = GetLastError();
	if (code == ERROR_NOT_FOUND) return true;
	if (error) *error = windows_error_message(code);
	return false;
#elif defined(__APPLE__)
	return delete_keychain_secret(credential_target, error);
#elif defined(WITH_LIBSECRET)
	return delete_secret_service_secret(credential_target, error);
#else
	(void)error;
	return true;
#endif
}

bool HasStoredApiKey() {
#ifdef _WIN32
	return !read_credential_key().empty();
#elif defined(__APPLE__)
	return !read_keychain_secret(credential_target).empty();
#elif defined(WITH_LIBSECRET)
	return !read_secret_service_secret(credential_target).empty();
#else
	return false;
#endif
}

namespace {

std::string cloudinary_transform(CloudinaryCredentials const& credentials,
	std::vector<unsigned char> const& image, std::string const& transform,
	std::function<bool()> const& cancelled) {
	if (!credentials.Complete()) throw Error("Cloudinary is not configured.");
	CurlHandle curl; std::string response;
	configure_common(curl, {}, &response, nullptr);
	std::string endpoint = std::string("https://api.cloudinary.com/v1_1/") + credentials.cloud_name + "/image/upload";
	curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str()); curl_easy_setopt(curl, CURLOPT_USERNAME, credentials.api_key.c_str());
	curl_easy_setopt(curl, CURLOPT_PASSWORD, credentials.api_secret.c_str()); curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
	curl_mime *mime = curl_mime_init(curl); if (!mime) throw Error("Cloudinary upload setup failed.");
	auto part = curl_mime_addpart(mime); curl_mime_name(part, "file"); curl_mime_filename(part, "selection.png"); curl_mime_type(part, "image/png"); curl_mime_data(part, reinterpret_cast<char const*>(image.data()), image.size());
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime); try { response = perform(curl, CurlHeaders{}, nullptr); } catch (...) { curl_mime_free(mime); throw; } curl_mime_free(mime);
	auto url = string_field(static_cast<json::Object const&>(parse_json(response)), "secure_url"); auto at = url.find("/upload/"); if (at == std::string::npos) throw Error("Cloudinary upload failed."); url.insert(at + 8, transform + "/");
	for (int i = 0; i < 30; ++i) { if (cancelled && cancelled()) throw Error("Cloudinary request cancelled."); CurlHandle get; std::string data; configure_common(get, {}, &data, nullptr); curl_easy_setopt(get, CURLOPT_URL, url.c_str()); auto rc = curl_easy_perform(get); long status = 0; curl_easy_getinfo(get, CURLINFO_RESPONSE_CODE, &status); if (rc == CURLE_OK && status / 100 == 2) return wxBase64Encode(data.data(), data.size()).ToStdString(); if (status != 420 && status != 423) throw Error(agi::format("Cloudinary HTTP %d", status)); std::this_thread::sleep_for(std::chrono::milliseconds(500)); }
	throw Error("Cloudinary processing timed out.");
}
}
std::string CloudinaryRemoveBackground(CloudinaryCredentials const& credentials,
	std::vector<unsigned char> const& image, std::function<bool()> const& cancelled) {
	return cloudinary_transform(credentials, image, "e_background_removal/f_png", cancelled);
}

std::string CloudinaryGenerativeRemove(CloudinaryCredentials const& credentials,
	std::vector<unsigned char> const& image, std::string const& prompt,
	std::function<bool()> const& cancelled) {
	CurlHandle escape_handle;
	char *escaped = curl_easy_escape(escape_handle, prompt.c_str(), static_cast<int>(prompt.size()));
	if (!escaped) throw Error("Cloudinary prompt encoding failed.");
	std::string transformation = std::string("e_gen_remove:prompt_") + escaped + "/f_png";
	curl_free(escaped);
	return cloudinary_transform(credentials, image, transformation, cancelled);
}

void TestCloudinaryConnection(CloudinaryCredentials const& credentials) {
	if (!credentials.Complete()) throw Error("Cloudinary is not configured.");
	CurlHandle curl;
	std::string response;
	configure_common(curl, {}, &response, nullptr);
	std::string url = std::string("https://api.cloudinary.com/v1_1/") +
		credentials.cloud_name + "/resources/image?max_results=1";
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERNAME, credentials.api_key.c_str());
	curl_easy_setopt(curl, CURLOPT_PASSWORD, credentials.api_secret.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
	(void)perform(curl, CurlHeaders{}, nullptr);
}

std::string GetCloudinarySecret() {
	{
		std::lock_guard<std::mutex> lock(cloudinary_secret_mutex);
		if (!session_cloudinary_secret.empty()) return session_cloudinary_secret;
	}
	auto environment = read_environment_cloudinary_secret();
	if (!environment.empty()) return environment;
#ifdef _WIN32
	auto stored = read_cloudinary_credential_secret();
#elif defined(__APPLE__)
	auto stored = read_keychain_secret(cloudinary_credential_target);
#elif defined(WITH_LIBSECRET)
	auto stored = read_secret_service_secret(cloudinary_credential_target);
#else
	std::string stored;
#endif
#if defined(_WIN32) || defined(__APPLE__) || defined(WITH_LIBSECRET)
	if (!stored.empty()) SetSessionCloudinarySecret(stored);
	return stored;
#else
	return {};
#endif
}

void SetSessionCloudinarySecret(std::string secret) {
	std::lock_guard<std::mutex> lock(cloudinary_secret_mutex);
	session_cloudinary_secret = std::move(secret);
}

void ClearSessionCloudinarySecret() {
	std::lock_guard<std::mutex> lock(cloudinary_secret_mutex);
	std::fill(session_cloudinary_secret.begin(), session_cloudinary_secret.end(), '\0');
	session_cloudinary_secret.clear();
}

bool StoreCloudinarySecret(std::string const& secret, std::string *error) {
	if (secret.empty()) { if (error) *error = "The Cloudinary API secret cannot be empty."; return false; }
#ifdef _WIN32
	CREDENTIALW credential{};
	credential.Type = CRED_TYPE_GENERIC;
	credential.TargetName = const_cast<wchar_t *>(cloudinary_credential_target);
	credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
	credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(secret.data()));
	credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
	credential.UserName = const_cast<wchar_t *>(L"Cloudinary API");
	if (!CredWriteW(&credential, 0)) { if (error) *error = windows_error_message(GetLastError()); return false; }
	SetSessionCloudinarySecret(secret);
	return true;
#elif defined(__APPLE__)
	if (!store_keychain_secret(cloudinary_credential_target, secret, error)) return false;
	SetSessionCloudinarySecret(secret);
	return true;
#elif defined(WITH_LIBSECRET)
	if (!store_secret_service_secret(cloudinary_credential_target,
		"Muteki Aegisub Cloudinary API secret", secret, error)) return false;
	SetSessionCloudinarySecret(secret);
	return true;
#else
	if (error) *error = "Secure Cloudinary secret storage is not supported on this platform; use CLOUDINARY_API_SECRET.";
	return false;
#endif
}

bool DeleteStoredCloudinarySecret(std::string *error) {
	ClearSessionCloudinarySecret();
#ifdef _WIN32
	if (CredDeleteW(cloudinary_credential_target, CRED_TYPE_GENERIC, 0)) return true;
	auto code = GetLastError();
	if (code == ERROR_NOT_FOUND) return true;
	if (error) *error = windows_error_message(code);
	return false;
#elif defined(__APPLE__)
	return delete_keychain_secret(cloudinary_credential_target, error);
#elif defined(WITH_LIBSECRET)
	return delete_secret_service_secret(cloudinary_credential_target, error);
#else
	(void)error;
	return true;
#endif
}

bool HasStoredCloudinarySecret() {
#ifdef _WIN32
	return !read_cloudinary_credential_secret().empty();
#elif defined(__APPLE__)
	return !read_keychain_secret(cloudinary_credential_target).empty();
#elif defined(WITH_LIBSECRET)
	return !read_secret_service_secret(cloudinary_credential_target).empty();
#else
	return false;
#endif
}

} // namespace ai
