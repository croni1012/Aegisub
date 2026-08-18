#include "dialog_folder_search.h"

#include "compat.h"
#include "dialog_manager.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "string_codec.h"
#include "subs_controller.h"
#include "theme.h"
#include "utils.h"

#include <libaegisub/ass/uuencode.h>
#include <libaegisub/util.h>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/regex/icu.hpp>
#include <unicode/uchar.h>
#include <unicode/utf8.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dcbuffer.h>
#include <wx/filepicker.h>
#include <wx/gauge.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

namespace {
wxDEFINE_EVENT(EVT_FOLDER_SEARCH_RESULT, wxThreadEvent);
using Range = std::pair<size_t, size_t>;

struct Matcher {
	boost::u32regex re;
	bool whole_word = false;

	static bool IsWordCharacter(UChar32 character) {
		if (character < 0) return false;
		auto category = U_GET_GC_MASK(character);
		return character == '_' || (category & (U_GC_L_MASK | U_GC_M_MASK | U_GC_N_MASK));
	}

	std::vector<Range> Find(std::string const& text) const {
		std::vector<Range> out;
		for (boost::u32regex_iterator<std::string::const_iterator> it(text.begin(), text.end(), re), end; it != end; ++it) {
			auto pos = static_cast<size_t>(it->position());
			auto len = static_cast<size_t>(it->length());
			if (whole_word) {
				UChar32 before = U_SENTINEL, after = U_SENTINEL;
				if (pos) {
					auto index = static_cast<int32_t>(pos);
					U8_PREV(text.data(), 0, index, before);
				}
				if (pos + len < text.size()) {
					auto index = static_cast<int32_t>(pos + len);
					U8_NEXT(text.data(), index, static_cast<int32_t>(text.size()), after);
				}
				if (IsWordCharacter(before) || IsWordCharacter(after)) continue;
			}
			out.emplace_back(pos, pos + len);
			if (!len && pos == text.size()) break;
		}
		return out;
	}
};

struct StyleFilter {
	std::unordered_set<std::string> include;
	std::unordered_set<std::string> exclude;

	bool Matches(std::string const& style) const {
		return (include.empty() || include.contains(style)) && !exclude.contains(style);
	}
};

std::unordered_set<std::string> parse_style_list(std::string const& value) {
	std::unordered_set<std::string> styles;
	size_t start = 0;
	while (start <= value.size()) {
		auto separator = value.find(',', start);
		auto end = separator == std::string::npos ? value.size() : separator;
		auto first = start;
		while (first < end && std::isspace(static_cast<unsigned char>(value[first]))) ++first;
		while (end > first && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
		if (end > first) styles.emplace(value.substr(first, end - first));
		if (separator == std::string::npos) break;
		start = separator + 1;
	}
	return styles;
}

std::string regex_escape(std::string const& value) {
	std::string out;
	for (char c : value) {
		if (std::string_view(R"(\.^$|()[]*+?{})").find(c) != std::string_view::npos) out += '\\';
		out += c;
	}
	return out;
}

Matcher make_matcher(std::string const& text, bool match_case, bool whole_word, bool regex) {
	auto pattern = regex ? text : regex_escape(text);
	int flags = boost::u32regex::perl;
	if (!match_case) flags |= boost::u32regex::icase;
	return {boost::make_u32regex(pattern, flags), whole_word};
}

struct ParsedLine {
	// `raw` is the untouched event line as it appears in the file. The other fields are
	// trimmed, tag-stripped and have the extradata prefix removed, so none of them can be
	// glued back into something a subtitle file would accept.
	std::string start, end, style, text, source, raw;
	std::vector<uint32_t> extra_ids;
	std::vector<Range> text_hits, source_hits;
	size_t line = 0;
};

struct ResultRow {
	std::filesystem::path file;
	std::string start, end, style, text, source, raw;
	std::vector<Range> text_hits, source_hits;
	size_t line = 0;
	bool openable = false;
};

struct ResultGroup {
	std::filesystem::path file;
	std::vector<ResultRow> rows;
	size_t match_line = 0;
};

struct ResultEvent {
	std::vector<ResultGroup> groups;
	size_t files = 0, hits = 0;
	bool done = false, cancelled = false;
	std::string error;
};

std::vector<uint32_t> extract_ids(std::string& text) {
	std::vector<uint32_t> ids;
	if (!text.starts_with("{=")) return ids;
	auto close = text.find('}');
	if (close == std::string::npos) return ids;
	for (size_t pos = 1; pos < close && text[pos] == '=';) {
		auto end = text.find('=', pos + 1);
		if (end == std::string::npos || end > close) end = close;
		try { ids.push_back(static_cast<uint32_t>(std::stoul(text.substr(pos + 1, end - pos - 1)))); }
		catch (...) { return {}; }
		pos = end;
	}
	text.erase(0, close + 1);
	return ids;
}

std::string extract_legacy_source(std::string& text) {
	constexpr std::string_view prefix = "{:Source Line: ";
	auto start = text.find(prefix);
	if (start == std::string::npos) return {};
	auto end = text.find('}', start + prefix.size());
	if (end == std::string::npos) return {};
	auto source = text.substr(start + prefix.size(), end - start - prefix.size());
	text.erase(start, end - start + 1);
	return source;
}

std::optional<ParsedLine> parse_event(std::string const& raw, size_t line_number) {
	std::string_view data;
	if (raw.starts_with("Dialogue:")) data = std::string_view(raw).substr(9);
	else if (raw.starts_with("Comment:")) data = std::string_view(raw).substr(8);
	else return {};
	while (!data.empty() && std::isspace(static_cast<unsigned char>(data.front()))) data.remove_prefix(1);
	std::vector<std::string_view> fields;
	size_t start = 0;
	for (int i = 0; i < 9; ++i) {
		auto comma = data.find(',', start);
		if (comma == std::string_view::npos) return {};
		fields.push_back(data.substr(start, comma - start));
		start = comma + 1;
	}
	fields.push_back(data.substr(start));
	auto trim = [](std::string_view v) {
		while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.remove_prefix(1);
		while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.remove_suffix(1);
		return std::string(v);
	};
	ParsedLine out;
	out.raw = raw;
	out.start = trim(fields[1]); out.end = trim(fields[2]); out.style = trim(fields[3]); out.line = line_number;
	auto text = std::string(fields[9]);
	out.extra_ids = extract_ids(text);
	out.source = agi::util::clean_ass_text(extract_legacy_source(text));
	out.text = agi::util::clean_ass_text(text);
	return out;
}

void parse_extra(std::string const& raw, std::unordered_map<uint32_t, std::string>& sources) {
	if (!raw.starts_with("Data:")) return;
	auto first = raw.find(','), second = first == std::string::npos ? first : raw.find(',', first + 1);
	if (first == std::string::npos || second == std::string::npos || second + 1 >= raw.size()) return;
	uint32_t id;
	try { id = static_cast<uint32_t>(std::stoul(raw.substr(5, first - 5))); }
	catch (...) { return; }
	if (inline_string_decode(std::string_view(raw).substr(first + 1, second - first - 1)) != "_aegi_source_line") return;
	auto value = raw.substr(second + 2);
	if (raw[second + 1] == 'e') sources[id] = inline_string_decode(value);
	else if (raw[second + 1] == 'u') {
		auto decoded = agi::ass::UUDecode(value.data(), value.data() + value.size());
		sources[id] = std::string(decoded.begin(), decoded.end());
	}
}

std::vector<ResultGroup> search_file(std::filesystem::path const& file, Matcher const& matcher, StyleFilter const& style_filter,
	std::atomic<bool> const& cancelled, size_t& hit_count) {
	std::ifstream stream(file, std::ios::binary);
	if (!stream) return {};
	enum class Section { other, events, extra } section = Section::other;
	std::vector<ParsedLine> lines;
	std::unordered_map<uint32_t, std::string> sources;
	std::string raw;
	while (!cancelled.load(std::memory_order_relaxed) && std::getline(stream, raw)) {
		if (!raw.empty() && raw.back() == '\r') raw.pop_back();
		if (raw.starts_with("\xEF\xBB\xBF")) raw.erase(0, 3);
		if (raw.size() > 1 && raw.front() == '[' && raw.back() == ']') {
			auto name = boost::algorithm::to_lower_copy(raw);
			section = name == "[events]" ? Section::events : name == "[aegisub extradata]" ? Section::extra : Section::other;
			continue;
		}
		if (section == Section::events) {
			if (auto line = parse_event(raw, lines.size())) {
				if (style_filter.Matches(line->style)) line->text_hits = matcher.Find(line->text);
				lines.push_back(std::move(*line));
			}
		}
		else if (section == Section::extra) parse_extra(raw, sources);
	}
	if (cancelled.load(std::memory_order_relaxed)) return {};
	for (auto& line : lines) {
		if (line.source.empty()) for (auto id : line.extra_ids) {
			auto found = sources.find(id);
			if (found != sources.end()) { line.source = agi::util::clean_ass_text(found->second); break; }
		}
		if (style_filter.Matches(line.style)) line.source_hits = matcher.Find(line.source);
	}
	std::vector<size_t> match_lines;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (lines[i].text_hits.empty() && lines[i].source_hits.empty()) continue;
		match_lines.push_back(i); ++hit_count;
	}
	if (!hit_count) return {};
	std::vector<ResultGroup> out;
	out.reserve(match_lines.size());
	for (auto match : match_lines) {
		ResultGroup group;
		group.file = file;
		group.match_line = lines[match].line;
		for (size_t i = match > 2 ? match - 2 : 0; i < std::min(lines.size(), match + 3); ++i) {
			auto const& line = lines[i];
			group.rows.push_back({file, line.start, line.end, line.style, line.text, line.source, line.raw,
				line.text_hits, line.source_hits, line.line, i == match});
		}
		out.push_back(std::move(group));
	}
	return out;
}

std::vector<std::pair<int, int>> char_ranges(std::string const& text, std::vector<Range> const& ranges) {
	std::vector<std::pair<int, int>> out;
	for (auto [a, b] : ranges) {
		a = std::min(a, text.size()); b = std::min(b, text.size());
		out.emplace_back(to_wx(std::string_view(text).substr(0, a)).length(), to_wx(std::string_view(text).substr(0, b)).length());
	}
	return out;
}

struct DisplayRow {
	wxString start, end, style, text, source, raw;
	std::vector<std::pair<int, int>> text_hits, source_hits;
	std::filesystem::path file;
	size_t line;
	bool openable;
	size_t id;
};

struct DisplayMatch {
	std::vector<DisplayRow> rows;
	size_t match_line = 0;
	int y = 0;
};

struct DisplayFile {
	wxString path;
	std::vector<DisplayMatch> matches;
	int y = 0;
};

class ResultsView final : public wxScrolledWindow {
	std::vector<DisplayFile> files;
	std::unordered_set<size_t> selected;
	std::function<void(wxString const&, wxString const&)> selection_changed;
	std::function<void(std::filesystem::path const&, size_t)> open_requested;
	size_t next_row_id = 1;
	int content_height = 0;

	int Margin() const { return FromDIP(8); }
	int FileHeight() const { return FromDIP(34); }
	int MatchHeight() const { return FromDIP(34); }
	int ColumnsHeight() const { return FromDIP(27); }
	int RowHeight() const { return FromDIP(27); }
	int Gap() const { return FromDIP(10); }
	int ContentWidth() const { return std::max(GetClientSize().x, FromDIP(1050)); }

	wxRect MatchButton(DisplayMatch const& match) const {
		return wxRect(ContentWidth() - Margin() - FromDIP(238), match.y + FromDIP(4), FromDIP(230), MatchHeight() - FromDIP(8));
	}

	void NotifySelection() {
		wxString text, raw;
		bool any = false;
		for (auto const& file : files) for (auto const& match : file.matches) for (auto const& row : match.rows) {
			if (!selected.contains(row.id)) continue;
			// An empty line is still a selected line, so the separator is driven by whether
			// anything came before rather than by the accumulated text being non-empty.
			if (any) { text += "\n"; raw += "\n"; }
			text += row.text; raw += row.raw; any = true;
		}
		if (selection_changed) selection_changed(text, raw);
	}

	void DrawTextCell(wxDC& dc, wxRect const& rect, wxString const& text,
		std::vector<std::pair<int, int>> const* ranges, bool is_selected) {
		dc.SetClippingRegion(rect);
		int x = rect.x + FromDIP(5), y = rect.y + std::max(0, (rect.height - dc.GetCharHeight()) / 2), pos = 0;
		auto draw = [&](wxString const& part, bool mark) {
			if (part.empty()) return;
			auto width = dc.GetTextExtent(part).x;
			if (mark) {
				dc.SetBrush(wxBrush(app_theme::Colour(is_selected ? "UI/Match Highlight Selected" : "UI/Match Highlight")));
				dc.SetPen(*wxTRANSPARENT_PEN); dc.DrawRectangle(x, y, width, dc.GetCharHeight());
			}
			dc.DrawText(part, x, y); x += width;
		};
		if (ranges) for (auto [start, end] : *ranges) {
			start = std::clamp(start, pos, static_cast<int>(text.length()));
			end = std::clamp(end, start, static_cast<int>(text.length()));
			draw(text.Mid(pos, start - pos), false); draw(text.Mid(start, end - start), true); pos = end;
		}
		draw(text.Mid(pos), false); dc.DestroyClippingRegion();
	}

	void OnPaint(wxPaintEvent&) {
		wxAutoBufferedPaintDC dc(this); PrepareDC(dc);
		dc.SetBackground(wxBrush(app_theme::Colour("UI/Background"))); dc.Clear();
		int visible_top; CalcUnscrolledPosition(0, 0, nullptr, &visible_top);
		int visible_bottom = visible_top + GetClientSize().y;
		int width = ContentWidth(), margin = Margin();
		auto normal_font = GetFont(), bold_font = normal_font; bold_font.MakeBold();
		auto first_file = std::upper_bound(files.begin(), files.end(), visible_top,
			[](int y, DisplayFile const& file) { return y < file.y; });
		if (first_file != files.begin()) --first_file;
		for (auto file_it = first_file; file_it != files.end(); ++file_it) {
			auto const& file = *file_it;
			if (file.y > visible_bottom) break;
			if (file.y + FileHeight() >= visible_top) {
				dc.SetBrush(wxBrush(app_theme::Colour("UI/File Header"))); dc.SetPen(*wxTRANSPARENT_PEN);
				dc.DrawRoundedRectangle(margin, file.y, width - margin * 2, FileHeight(), FromDIP(4));
				dc.SetFont(bold_font); dc.SetTextForeground(app_theme::Colour("UI/File Header Text"));
				dc.DrawText(file.path, margin + FromDIP(10), file.y + (FileHeight() - dc.GetCharHeight()) / 2);
			}
			auto first_match = std::upper_bound(file.matches.begin(), file.matches.end(), visible_top,
				[](int y, DisplayMatch const& match) { return y < match.y; });
			if (first_match != file.matches.begin()) --first_match;
			for (auto match_it = first_match; match_it != file.matches.end(); ++match_it) {
				auto const& match = *match_it;
				auto match_index = static_cast<size_t>(match_it - file.matches.begin());
				int bottom = match.y + MatchHeight() + ColumnsHeight() + static_cast<int>(match.rows.size()) * RowHeight();
				if (bottom < visible_top) continue;
				if (match.y > visible_bottom) break;
				dc.SetBrush(wxBrush(app_theme::Colour("UI/Match Header"))); dc.SetPen(wxPen(app_theme::Colour("UI/Row Border")));
				dc.DrawRectangle(margin, match.y, width - margin * 2, MatchHeight());
				dc.SetFont(bold_font); dc.SetTextForeground(app_theme::Colour("UI/Text"));
				dc.DrawText(wxString::Format(_("Match %zu - Line %zu"), match_index + 1, match.match_line + 1),
					margin + FromDIP(8), match.y + (MatchHeight() - dc.GetCharHeight()) / 2);
				auto button = MatchButton(match);
				dc.SetBrush(wxBrush(app_theme::Colour("UI/Button"))); dc.SetPen(wxPen(app_theme::Colour("UI/Button Border")));
				dc.DrawRoundedRectangle(button, FromDIP(3));
				auto open_label = _("Open subtitle at match");
				auto extent = dc.GetTextExtent(open_label);
				dc.DrawText(open_label, button.x + (button.width - extent.x) / 2, button.y + (button.height - extent.y) / 2);

				int columns_y = match.y + MatchHeight();
				int fixed = FromDIP(100 + 100 + 145), flexible = std::max(FromDIP(360), width - margin * 2 - fixed);
				int text_width = flexible / 2;
				int xs[] = {margin, margin + FromDIP(100), margin + FromDIP(200), margin + FromDIP(345), margin + FromDIP(345) + text_width, width - margin};
				wxString labels[] = {_("Start"), _("End"), _("Style"), _("Text"), _("Source Line")};
				dc.SetBrush(wxBrush(app_theme::Colour("UI/Column Header"))); dc.SetPen(wxPen(app_theme::Colour("UI/Row Border")));
				dc.DrawRectangle(margin, columns_y, width - margin * 2, ColumnsHeight());
				dc.SetFont(bold_font);
				for (int col = 0; col < 5; ++col) {
					dc.DrawLine(xs[col], columns_y, xs[col], bottom);
					dc.DrawText(labels[col], xs[col] + FromDIP(5), columns_y + (ColumnsHeight() - dc.GetCharHeight()) / 2);
				}
				dc.DrawLine(xs[5], columns_y, xs[5], bottom); dc.SetFont(normal_font);
				for (size_t row_index = 0; row_index < match.rows.size(); ++row_index) {
					auto const& row = match.rows[row_index]; int y = columns_y + ColumnsHeight() + static_cast<int>(row_index) * RowHeight();
					bool is_selected = selected.contains(row.id);
					dc.SetBrush(wxBrush(app_theme::Colour(is_selected ? "UI/Selection" : row.openable ? "UI/Openable Row" : "UI/Row")));
					dc.SetPen(wxPen(app_theme::Colour("UI/Row Border"))); dc.DrawRectangle(margin, y, width - margin * 2, RowHeight());
					dc.SetTextForeground(app_theme::Colour(is_selected ? "UI/Selection Text" : "UI/Text"));
					DrawTextCell(dc, wxRect(xs[0], y, xs[1] - xs[0], RowHeight()), row.start, nullptr, is_selected);
					DrawTextCell(dc, wxRect(xs[1], y, xs[2] - xs[1], RowHeight()), row.end, nullptr, is_selected);
					DrawTextCell(dc, wxRect(xs[2], y, xs[3] - xs[2], RowHeight()), row.style, nullptr, is_selected);
					DrawTextCell(dc, wxRect(xs[3], y, xs[4] - xs[3], RowHeight()), row.text, &row.text_hits, is_selected);
					DrawTextCell(dc, wxRect(xs[4], y, xs[5] - xs[4], RowHeight()), row.source, &row.source_hits, is_selected);
				}
			}
		}
	}

	void OnLeftDown(wxMouseEvent& event) {
		int x, y; CalcUnscrolledPosition(event.GetX(), event.GetY(), &x, &y);
		if (files.empty()) return;
		auto file_it = std::upper_bound(files.begin(), files.end(), y,
			[](int point_y, DisplayFile const& file) { return point_y < file.y; });
		if (file_it == files.begin()) return;
		--file_it;
		auto match_it = std::upper_bound(file_it->matches.begin(), file_it->matches.end(), y,
			[](int point_y, DisplayMatch const& match) { return point_y < match.y; });
		if (match_it == file_it->matches.begin()) return;
		--match_it;
		auto const& match = *match_it;
		if (MatchButton(match).Contains(x, y)) {
			if (open_requested) open_requested(match.rows.front().file, match.match_line);
			return;
		}
		int rows_y = match.y + MatchHeight() + ColumnsHeight();
		if (y < rows_y || y >= rows_y + static_cast<int>(match.rows.size()) * RowHeight()) return;
		auto& row = match.rows[(y - rows_y) / RowHeight()];
		if (!event.ControlDown()) selected.clear();
		if (event.ControlDown() && selected.contains(row.id)) selected.erase(row.id); else selected.insert(row.id);
		NotifySelection(); Refresh(); return;
	}

public:
	explicit ResultsView(wxWindow *parent) : wxScrolledWindow(parent, -1, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxHSCROLL | wxBORDER_SIMPLE) {
		SetBackgroundStyle(wxBG_STYLE_PAINT); SetScrollRate(FromDIP(12), FromDIP(12)); content_height = Margin();
		Bind(wxEVT_PAINT, &ResultsView::OnPaint, this); Bind(wxEVT_LEFT_DOWN, &ResultsView::OnLeftDown, this);
		Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { SetVirtualSize(ContentWidth(), content_height); Refresh(); event.Skip(); });
	}
	void SetSelectionChanged(std::function<void(wxString const&, wxString const&)> callback) { selection_changed = std::move(callback); }
	void SetOpenRequested(std::function<void(std::filesystem::path const&, size_t)> callback) { open_requested = std::move(callback); }
	void ClearResults() {
		files.clear(); selected.clear(); next_row_id = 1; content_height = Margin(); SetVirtualSize(ContentWidth(), content_height); Refresh(); NotifySelection();
	}
	void Append(std::vector<ResultGroup>& groups) {
		for (auto& group : groups) {
			auto path = to_wx(agi::fs::path(group.file).string());
			if (files.empty() || files.back().path != path) {
				DisplayFile file; file.path = path; file.y = content_height; content_height += FileHeight(); files.push_back(std::move(file));
			}
			DisplayMatch match; match.y = content_height; match.match_line = group.match_line;
			for (auto& item : group.rows) {
				match.rows.push_back({to_wx(item.start), to_wx(item.end), to_wx(item.style), to_wx(item.text), to_wx(item.source), to_wx(item.raw),
					char_ranges(item.text, item.text_hits), char_ranges(item.source, item.source_hits), std::move(item.file), item.line, item.openable, next_row_id++});
			}
			content_height += MatchHeight() + ColumnsHeight() + static_cast<int>(match.rows.size()) * RowHeight() + Gap();
			files.back().matches.push_back(std::move(match));
		}
		SetVirtualSize(ContentWidth(), content_height + Margin()); Refresh();
	}
};

class DialogFolderSearch final : public wxDialog {
	wxDirPickerCtrl *folder;
	wxTextCtrl *query, *include_styles, *exclude_styles, *selected_text;
	wxCheckBox *match_case, *whole_word, *regex;
	wxButton *search, *stop, *copy, *copy_full;
	// The full event lines are not shown anywhere, so the selection has to keep them.
	wxString selected_raw;
	wxGauge *progress;
	wxStaticText *status;
	ResultsView *results;
	wxTimer pulse;
	std::thread worker;
	std::atomic<bool> cancelled{false};
	bool running = false;
	size_t files = 0, hits = 0;

	void stop_worker() { cancelled = true; if (worker.joinable()) worker.join(); }
	void set_running(bool value) {
		running = value; search->Enable(!value); stop->Enable(value); folder->Enable(!value); query->Enable(!value);
		include_styles->Enable(!value); exclude_styles->Enable(!value);
		match_case->Enable(!value); whole_word->Enable(!value); regex->Enable(!value);
		if (value) pulse.Start(100); else { pulse.Stop(); progress->SetValue(0); }
	}
	void set_status(wxString const& value) {
		status->SetLabel(value);
		Layout();
	}
	void clear_results() {
		results->ClearResults(); selected_text->Clear(); selected_raw.clear(); copy->Disable(); copy_full->Disable();
	}
	void append_groups(std::vector<ResultGroup>& groups) {
		results->Append(groups);
	}
	void start_search() {
		if (running) return;
		auto root = std::filesystem::path(from_wx(folder->GetPath())); auto value = from_wx(query->GetValue());
		if (root.empty() || !std::filesystem::is_directory(root)) { wxMessageBox(_("Please select an existing folder."), _("Find in Folder"), wxOK | wxICON_WARNING, this); return; }
		if (value.empty()) { wxMessageBox(_("Please enter the text to find."), _("Find in Folder"), wxOK | wxICON_WARNING, this); return; }
		Matcher matcher;
		try { matcher = make_matcher(value, match_case->GetValue(), whole_word->GetValue(), regex->GetValue()); }
		catch (std::exception const& e) { wxMessageBox(fmt_tl("Invalid regular expression:\n%s", e.what()), _("Find in Folder"), wxOK | wxICON_ERROR, this); return; }
		StyleFilter style_filter{parse_style_list(from_wx(include_styles->GetValue())), parse_style_list(from_wx(exclude_styles->GetValue()))};
		stop_worker(); cancelled = false; clear_results(); files = hits = 0;
		set_status(_("Searching...")); set_running(true);
		worker = std::thread([this, root = std::move(root), matcher = std::move(matcher), style_filter = std::move(style_filter)] {
			auto result = std::make_shared<ResultEvent>();
			try {
				std::error_code error;
				std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, error), end;
				for (; it != end && !cancelled; it.increment(error)) {
					if (error) { error.clear(); continue; }
					if (!it->is_regular_file(error) || boost::algorithm::to_lower_copy(it->path().extension().string()) != ".ass") continue;
					++result->files; size_t file_hits = 0;
					auto groups = search_file(it->path(), matcher, style_filter, cancelled, file_hits); result->hits += file_hits;
					if (!groups.empty() || result->files % 25 == 0) {
						result->groups = std::move(groups); auto event = new wxThreadEvent(EVT_FOLDER_SEARCH_RESULT);
						event->SetPayload(result); wxQueueEvent(this, event); result = std::make_shared<ResultEvent>();
					}
				}
			}
			catch (std::exception const& e) { result->error = e.what(); }
			result->cancelled = cancelled; result->done = true; auto event = new wxThreadEvent(EVT_FOLDER_SEARCH_RESULT);
			event->SetPayload(result); wxQueueEvent(this, event);
		});
	}
	void on_result(wxThreadEvent& event) {
		auto result = event.GetPayload<std::shared_ptr<ResultEvent>>(); files += result->files; hits += result->hits;
		if (!result->groups.empty()) append_groups(result->groups);
		set_status(wxString::Format(_("%zu subtitle files searched, %zu matches found"), files, hits));
		if (!result->done) return;
		if (worker.joinable()) {
			worker.join();
		}
		set_running(false);
		if (!result->error.empty()) wxMessageBox(fmt_tl("Folder search failed:\n%s", result->error), _("Find in Folder"), wxOK | wxICON_ERROR, this);
		else if (result->cancelled) set_status(wxString::Format(_("Search stopped: %zu subtitle files searched, %zu matches found"), files, hits));
		else set_status(wxString::Format(_("Search complete: %zu subtitle files searched, %zu matches found"), files, hits));
	}

public:
	explicit DialogFolderSearch(agi::Context *c)
	: wxDialog(c->parent, -1, _("Find in Folder"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER), pulse(this) {
		folder = new wxDirPickerCtrl(this, -1, to_wx(c->subsController->Filename().parent_path().string()), _("Select a folder to search"),
			wxDefaultPosition, wxDefaultSize, wxDIRP_USE_TEXTCTRL | wxDIRP_DIR_MUST_EXIST);
		folder->GetPickerCtrl()->SetLabel(_("Browse"));
		query = new wxTextCtrl(this, -1, {}, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
		include_styles = new wxTextCtrl(this, -1, {}, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
		exclude_styles = new wxTextCtrl(this, -1, {}, wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
		match_case = new wxCheckBox(this, -1, _("&Match case")); whole_word = new wxCheckBox(this, -1, _("Match &whole word"));
		regex = new wxCheckBox(this, -1, _("&Use regular expressions")); search = new wxButton(this, -1, _("&Search")); search->SetDefault();
		stop = new wxButton(this, -1, _("S&top")); stop->Disable(); progress = new wxGauge(this, -1, 100); status = new wxStaticText(this, -1, _("Ready."));
		app_theme::StyleProgress(progress);
		progress->SetMinSize(FromDIP(wxSize(180, -1))); status->SetMinSize(FromDIP(wxSize(500, -1)));
		results = new ResultsView(this);
		results->SetSelectionChanged([this](wxString const& text, wxString const& raw) {
			selected_text->SetValue(text); selected_raw = raw;
			copy->Enable(!text.empty()); copy_full->Enable(!raw.empty());
		});
		results->SetOpenRequested([](std::filesystem::path const& file, size_t line) { LaunchAegisubAtLine(agi::fs::path(file), line); });
		selected_text = new wxTextCtrl(this, -1, {}, wxDefaultPosition, FromDIP(wxSize(-1, 85)), wxTE_MULTILINE | wxTE_READONLY);
		copy = new wxButton(this, -1, _("Copy to &Clipboard")); copy->Disable();
		copy_full = new wxButton(this, -1, _("Copy to Clipboard (&full line)")); copy_full->Disable();
		auto form = new wxFlexGridSizer(2, FromDIP(6), FromDIP(8)); form->AddGrowableCol(1, 1);
		form->Add(new wxStaticText(this, -1, _("Folder:")), wxSizerFlags().CenterVertical()); form->Add(folder, wxSizerFlags(1).Expand());
		form->Add(new wxStaticText(this, -1, _("Find what:")), wxSizerFlags().CenterVertical()); form->Add(query, wxSizerFlags(1).Expand());
		form->Add(new wxStaticText(this, -1, _("Include styles (comma-separated):")), wxSizerFlags().CenterVertical()); form->Add(include_styles, wxSizerFlags(1).Expand());
		form->Add(new wxStaticText(this, -1, _("Exclude styles (comma-separated):")), wxSizerFlags().CenterVertical()); form->Add(exclude_styles, wxSizerFlags(1).Expand());
		auto options = new wxBoxSizer(wxHORIZONTAL); options->Add(match_case, wxSizerFlags().Border(wxRIGHT)); options->Add(whole_word, wxSizerFlags().Border(wxRIGHT));
		options->Add(regex, wxSizerFlags().Border(wxRIGHT)); options->AddStretchSpacer(); options->Add(search, wxSizerFlags().Border(wxRIGHT)); options->Add(stop);
		auto progress_row = new wxBoxSizer(wxHORIZONTAL); progress_row->Add(progress, wxSizerFlags(1).Expand().Border(wxRIGHT)); progress_row->Add(status, wxSizerFlags().CenterVertical());
		auto buttons = new wxBoxSizer(wxHORIZONTAL); buttons->Add(copy, wxSizerFlags().Border(wxRIGHT));
		buttons->Add(copy_full, wxSizerFlags().Border(wxRIGHT));
		buttons->AddStretchSpacer(); buttons->Add(new wxButton(this, wxID_CANCEL, _("Close")));
		auto main = new wxBoxSizer(wxVERTICAL); main->Add(form, wxSizerFlags().Expand().Border()); main->Add(options, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(progress_row, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(new wxStaticText(this, -1, _("Selected subtitle text:")), wxSizerFlags().Border(wxLEFT | wxRIGHT));
		main->Add(selected_text, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM)); main->Add(buttons, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(results, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		SetSizer(main); SetMinSize(FromDIP(wxSize(850, 560))); SetSize(FromDIP(wxSize(1150, 760))); CenterOnParent();
		Bind(EVT_FOLDER_SEARCH_RESULT, &DialogFolderSearch::on_result, this); Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (running) progress->Pulse(); });
		search->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_search(); }); query->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { start_search(); });
		include_styles->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { start_search(); }); exclude_styles->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { start_search(); });
		stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancelled = true; set_status(_("Stopping search...")); stop->Disable(); });
		copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SetClipboard(from_wx(selected_text->GetValue())); });
		copy_full->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { SetClipboard(from_wx(selected_raw)); });
		query->SetFocus();
	}
	~DialogFolderSearch() override { pulse.Stop(); stop_worker(); }
};
}

void ShowFolderSearchDialog(agi::Context *context) { context->dialog->Show<DialogFolderSearch>(context); }
