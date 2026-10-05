// Copyright (c) 2026, 伤感咩吖
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

/// @file subtitle_format_ttml.cpp
/// @brief Reading TTML (Timed Text Markup Language) / DFXP subtitles.
/// Paragraphs (<p>) map to dialogue lines; word-level <span begin=..> timing
/// maps to ASS \kf sweeping karaoke durations; <br/> becomes \N. Namespaced
/// documents (any prefix, or a default namespace) are handled by comparing
/// local names.
/// @ingroup subtitle_io

#include "subtitle_format_ttml.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "options.h"

#include <libaegisub/ass/time.h>

#include <wx/xml/xml.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include <boost/algorithm/string/trim.hpp>

namespace {
// Use the classic locale on every platform: strtod follows LC_NUMERIC,
// while TTML always uses '.' irrespective of the user's language settings.
bool ParseDouble(std::string_view str, double& out) {
	if (str.empty() || str.size() >= 64) return false;
	std::istringstream stream{std::string(str)};
	stream.imbue(std::locale::classic());
	stream >> std::noskipws >> out;
	return !stream.fail() && stream.eof() && std::isfinite(out) && out >= 0;
}

int64_t Milliseconds(double amount, double scale) {
	double ms = amount * scale;
	if (!std::isfinite(ms) || ms < 0 || ms > std::numeric_limits<int>::max()) return -1;
	return static_cast<int64_t>(std::llround(ms));
}

// Parse a TTML clock-time or offset-time value to milliseconds.
// Supports "HH:MM:SS.mmm", "MM:SS.mmm", "SS.mmm", "123ms", "4.5s", "2m", "1h".
int64_t ParseTTMLTime(std::string_view value) {
	// string_view has no erase(), so trim by moving the ends inward.
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n'))
		value.remove_prefix(1);
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n'))
		value.remove_suffix(1);
	if (value.empty()) return -1;

	// Offset time with a metric suffix.
	if (!value.empty() && !isdigit(static_cast<unsigned char>(value.back()))) {
		std::string_view suffix = value.substr(value.size() - 1);
		std::string_view num = value.substr(0, value.size() - 1);
		if (suffix == "s" && !num.empty() && num.back() == 'm') {
			// "ms"
			num = value.substr(0, value.size() - 2);
			suffix = "ms";
		}
		double amount = 0.0;
		if (!ParseDouble(num, amount))
			return -1;
		if (suffix == "ms") return Milliseconds(amount, 1);
		if (suffix == "s") return Milliseconds(amount, 1000);
		if (suffix == "m") return Milliseconds(amount, 60000);
		if (suffix == "h") return Milliseconds(amount, 3600000);
		return -1;
	}

	// Clock time with colons. Fraction is separated by '.' (or ',' per spec).
	size_t first_colon = value.find(':');
	if (first_colon == std::string::npos) {
		// Bare seconds like "12.5"
		double seconds = 0.0;
		if (!ParseDouble(value, seconds))
			return -1;
		return Milliseconds(seconds, 1000);
	}

	// Colon-separated clock time: only integral hour/minute components,
	// and a seconds component with an optional decimal fraction.
	size_t last_colon = value.find_last_of(':');
	std::string seconds_part(value.substr(last_colon + 1));
	std::replace(seconds_part.begin(), seconds_part.end(), ',', '.');
	double seconds = 0;
	if (!ParseDouble(seconds_part, seconds) || seconds >= 60) return -1;
	auto integer = [](std::string_view part, int64_t& out) {
		if (part.empty() || part.find_first_not_of("0123456789") != std::string_view::npos) return false;
		auto parsed = std::from_chars(part.data(), part.data() + part.size(), out);
		return parsed.ec == std::errc{} && parsed.ptr == part.data() + part.size();
	};
	int64_t hours = 0, minutes = 0;
	if (first_colon == last_colon) {
		if (!integer(value.substr(0, first_colon), minutes)) return -1;
	}
	else {
		if (!integer(value.substr(0, first_colon), hours)
			|| !integer(value.substr(first_colon + 1, last_colon - first_colon - 1), minutes)
			|| minutes >= 60) return -1;
	}
	return Milliseconds(static_cast<double>(hours) * 3600 + static_cast<double>(minutes) * 60 + seconds, 1000);
}

// Local name of an XML node with any namespace prefix stripped.
std::string LocalName(wxXmlNode const* node) {
	std::string name = node->GetName().ToStdString(wxConvUTF8);
	auto colon = name.find(':');
	return colon == std::string::npos ? name : name.substr(colon + 1);
}

bool IsElement(wxXmlNode const* node, std::string_view local) {
	return LocalName(node) == local;
}

struct TtmlSegment {
	int64_t begin_ms = 0;
	int64_t end_ms = -1;  // -1 when the span carries no end attribute
	std::string text;
	bool bg = false;      // inside a ttm:role="x-bg" background-vocal wrapper
};

struct TtmlParagraph {
	int64_t begin_ms = 0;
	int64_t end_ms = 0;
	std::string karaoke_text; // ASS text with \k segments when timed spans exist
	bool has_karaoke = false;
	std::vector<TtmlSegment> segments;
};

// Determine (begin, end) for a paragraph from begin/end/dur attributes.
bool ParseParagraphTimes(wxXmlNode const* p, int64_t& begin_ms, int64_t& end_ms) {
	std::string begin_str, end_str, dur_str;
	for (auto attr = p->GetAttributes(); attr; attr = attr->GetNext()) {
		std::string name = attr->GetName().ToStdString(wxConvUTF8);
		auto pos = name.find(':');
		if (pos != std::string::npos) name = name.substr(pos + 1);
		if (name == "begin") begin_str = attr->GetValue().ToStdString(wxConvUTF8);
		else if (name == "end") end_str = attr->GetValue().ToStdString(wxConvUTF8);
		else if (name == "dur") dur_str = attr->GetValue().ToStdString(wxConvUTF8);
	}

	begin_ms = ParseTTMLTime(begin_str);
	if (begin_ms < 0) return false;

	if (!end_str.empty()) {
		end_ms = ParseTTMLTime(end_str);
		if (end_ms < 0) return false;
	}
	else if (!dur_str.empty()) {
		int64_t dur = ParseTTMLTime(dur_str);
		if (dur < 0 || dur > std::numeric_limits<int>::max() - begin_ms) return false;
		end_ms = begin_ms + dur;
	}
	else {
		end_ms = begin_ms;
	}

	if (end_ms < begin_ms) end_ms = begin_ms;
	return true;
}

// Walk a paragraph's children, accumulating plain text and <span>-scoped
// karaoke segments. Runs of text between spans attach to the most recent
// span (or the paragraph preamble when no span has been seen yet).
// Collect all visible text under a node in document order, turning <br/>
// into \N and flattening nested elements (spans inside spans).
std::string CollectVisibleText(wxXmlNode const* node) {
	std::string out;
	for (auto child = node->GetChildren(); child; child = child->GetNext()) {
		switch (child->GetType()) {
			case wxXML_TEXT_NODE:
			case wxXML_CDATA_SECTION_NODE:
				out += child->GetContent().ToStdString(wxConvUTF8);
				break;
			case wxXML_ELEMENT_NODE:
				if (IsElement(child, "br"))
					out += "\\N";
				else
					out += CollectVisibleText(child);
				break;
			default:
				break;
		}
	}
	return out;
}

void BuildParagraphText(wxXmlNode *p,
	std::string& plain, bool& has_karaoke,
	std::vector<TtmlSegment>& segments, bool bg_voice = false) {
	for (auto node = p->GetChildren(); node; node = node->GetNext()) {
		switch (node->GetType()) {
			case wxXML_TEXT_NODE:
			case wxXML_CDATA_SECTION_NODE: {
				std::string text = node->GetContent().ToStdString(wxConvUTF8);
				if (has_karaoke && !segments.empty())
					segments.back().text += text;
				else
					plain += text;
				break;
			}
			case wxXML_ELEMENT_NODE: {
				if (IsElement(node, "br")) {
					if (has_karaoke && !segments.empty())
						segments.back().text += "\\N";
					else
						plain += "\\N";
					break;
				}
				if (IsElement(node, "span")) {
					std::string begin_attr, end_attr;
					bool role_bg = false;
					for (auto attr = node->GetAttributes(); attr; attr = attr->GetNext()) {
						std::string name = attr->GetName().ToStdString(wxConvUTF8);
						auto pos = name.find(':');
						if (pos != std::string::npos) name = name.substr(pos + 1);
						if (name == "begin") begin_attr = attr->GetValue().ToStdString(wxConvUTF8);
						else if (name == "end") end_attr = attr->GetValue().ToStdString(wxConvUTF8);
						else if (name == "role") {
							// Apple Music marks background vocals with
							// ttm:role="x-bg"; they sing alongside the lead.
							std::string role = attr->GetValue().ToStdString(wxConvUTF8);
							if (role.rfind("x-bg", 0) == 0) role_bg = true;
						}
					}
					bool voice_bg = bg_voice || role_bg;

					// Collect this span's own visible text. Nested spans are
					// flattened in document order — an untimed wrapper span
					// (e.g. ttm:role="x-bg" background vocals wrapping timed
					// word spans in Apple Music TTML) must not swallow its
					// children, and a timed span containing nested spans
					// (rare) uses its own begin for the whole run.
					std::string span_text = CollectVisibleText(node);
					int64_t span_begin = ParseTTMLTime(begin_attr);
					int64_t span_end = end_attr.empty() ? -1 : ParseTTMLTime(end_attr);
					if (span_begin >= 0 && !span_text.empty()) {
						has_karaoke = true;
						segments.push_back({span_begin, span_end, span_text, voice_bg});
					}
					else if (!span_text.empty()) {
						// Untimed wrapper: its timed children still need their
						// own segments, so recurse instead of flattening them
						// into plain text, propagating the x-bg voice flag.
						// Share the current text/segments so untimed wrapper text
						// is appended exactly once at its document position.
						BuildParagraphText(node, plain, has_karaoke, segments, voice_bg);
					}
					break;
				}
				// Other elements: recurse so metadata wrappers never eat text.
				BuildParagraphText(node, plain, has_karaoke, segments, bg_voice);
				break;
			}
			default:
				break;
		}
	}
}
}

TTMLSubtitleFormat::TTMLSubtitleFormat()
: SubtitleFormat("TTML Timed Text")
{
}

std::vector<std::string> TTMLSubtitleFormat::GetReadWildcards() const {
	return {"ttml", "dfxp", "xml"};
}

void TTMLSubtitleFormat::ReadFile(AssFile *target, agi::fs::path const& filename, agi::vfr::Framerate const&, const char *encoding) const {
	target->LoadDefault(false, OPT_GET("Subtitle Format/TTXT/Default Style Catalog")->GetString());

	wxXmlDocument doc;
	if (!doc.Load(wxString::FromUTF8(filename.string()))) throw SubtitleFormatParseError("Failed loading TTML XML file.");
	if (!doc.GetRoot() || !IsElement(doc.GetRoot(), "tt"))
		throw SubtitleFormatParseError("Invalid TTML file: root element is not <tt>.");

	std::vector<TtmlParagraph> paragraphs;

	// Depth-first search for <p> elements anywhere below the root (they live
	// under body/div in valid TTML, but real-world files vary).
	for (auto node = doc.GetRoot(); node; node = node->GetNext()) {
		if (node->GetType() != wxXML_ELEMENT_NODE) continue;

		// Iterative traversal from this top-level node.
		std::vector<wxXmlNode*> stack{node};
		while (!stack.empty()) {
			wxXmlNode *cur = stack.back();
			stack.pop_back();
			if (cur->GetType() != wxXML_ELEMENT_NODE) continue;

			if (IsElement(cur, "p")) {
				TtmlParagraph para;
				bool paragraph_timed = ParseParagraphTimes(cur, para.begin_ms, para.end_ms);
				if (!paragraph_timed) {
					bool has_begin = false;
					for (auto attr = cur->GetAttributes(); attr; attr = attr->GetNext()) {
						auto name = attr->GetName().ToStdString(wxConvUTF8);
						if (name == "begin" || name.ends_with(":begin")) has_begin = true;
					}
					if (has_begin) continue;
					// Apple Music exports songs without any timing at all
					// (itunes:timing="None"): every <p> is bare text. Import
					// them like the plain-text reader does — one untimed row
					// per paragraph — instead of failing the whole file.
					para.begin_ms = 0;
					para.end_ms = 0;
				}

				std::string plain;
				BuildParagraphText(cur, plain, para.has_karaoke, para.segments);

				boost::trim_left(plain);
				if (para.has_karaoke) {
					std::stable_sort(para.segments.begin(), para.segments.end(),
						[](TtmlSegment const& a, TtmlSegment const& b) { return a.begin_ms < b.begin_ms; });
					if (para.end_ms <= para.begin_ms) {
						// Untimed <p> whose word spans still carry timing:
						// derive the row's bounds from the spans themselves.
						if (!paragraph_timed) para.begin_ms = para.segments.front().begin_ms;
						para.end_ms = 0;
						for (auto const& seg : para.segments)
							para.end_ms = std::max(para.end_ms,
								seg.end_ms > seg.begin_ms ? seg.end_ms : std::min<int64_t>(seg.begin_ms + 500, std::numeric_limits<int>::max()));
						para.end_ms = std::max(para.end_ms, para.begin_ms);
					}
					auto karaoke_cs = [](int64_t ms) { return static_cast<int>((ms + 5) / 10); };
					// Word spans that carry their own end keep it (Apple Music
					// data does), so held notes keep their real length; others
					// run to the next word or the line end.
					auto build_karaoke = [&](std::vector<TtmlSegment> const& segs, int64_t line_begin, int64_t line_end) {
						std::string out;
						int64_t elapsed_cs = 0;
						for (size_t s = 0; s < segs.size(); ++s) {
							int64_t seg_begin = std::clamp(segs[s].begin_ms, line_begin, line_end);
							int64_t begin_cs = karaoke_cs(seg_begin - line_begin);
							if (begin_cs > elapsed_cs) {
								out += "{\\k" + std::to_string(begin_cs - elapsed_cs) + "}";
								elapsed_cs = begin_cs;
							}
							int64_t seg_end = segs[s].end_ms > segs[s].begin_ms ? segs[s].end_ms : line_end;
							if (s + 1 < segs.size()) seg_end = std::min(seg_end, segs[s + 1].begin_ms);
							int64_t end_cs = karaoke_cs(std::clamp(seg_end, seg_begin, line_end) - line_begin);
							int64_t dur_cs = std::max<int64_t>(end_cs - elapsed_cs, 0);
							out += "{\\kf" + std::to_string(dur_cs) + "}" + segs[s].text;
							elapsed_cs += dur_cs;
						}
						return out;
					};

					// Background vocals (x-bg) that enter while the lead voice
					// is still singing cannot share one gapless \kf chain —
					// sorting by begin scrambles the word order and the lead's
					// held notes get truncated at the harmony's first word.
					// Split them into their own dialogue row with their real
					// timings; ASS stacks simultaneous rows by itself.
					std::vector<TtmlSegment> lead_segs, bg_segs;
					for (auto const& seg : para.segments)
						(seg.bg ? bg_segs : lead_segs).push_back(seg);

					int64_t lead_last_end = 0;
					for (auto const& seg : lead_segs)
						lead_last_end = std::max(lead_last_end,
							seg.end_ms > seg.begin_ms ? seg.end_ms : seg.begin_ms);

					if (!bg_segs.empty() && !lead_segs.empty()
						&& bg_segs.front().begin_ms < lead_last_end) {
						TtmlParagraph harmony;
						harmony.begin_ms = std::max<int64_t>(para.begin_ms, bg_segs.front().begin_ms);
						harmony.end_ms = std::max(para.end_ms, harmony.begin_ms);
						harmony.karaoke_text = build_karaoke(bg_segs, harmony.begin_ms, harmony.end_ms);
						boost::trim(harmony.karaoke_text);
						para.karaoke_text = plain + build_karaoke(lead_segs, para.begin_ms, para.end_ms);
						boost::trim(para.karaoke_text);

						if (!harmony.karaoke_text.empty() && !para.karaoke_text.empty()) {
							paragraphs.push_back(std::move(para));
							paragraphs.push_back(std::move(harmony));
							continue;
						}
					}
					para.karaoke_text = plain + build_karaoke(para.segments, para.begin_ms, para.end_ms);
				}
				else {
					para.karaoke_text = plain;
				}

				boost::trim(para.karaoke_text);
				if (para.karaoke_text.empty()) continue;
				paragraphs.push_back(std::move(para));
				continue; // do not recurse into a <p>
			}

			// A LIFO stack must push siblings in reverse to visit them in
			// document order, especially for untimed/equal-time paragraphs.
			std::vector<wxXmlNode*> children;
			for (auto child = cur->GetChildren(); child; child = child->GetNext())
				children.push_back(child);
			stack.insert(stack.end(), children.rbegin(), children.rend());
		}
	}

	if (paragraphs.empty())
		throw SubtitleFormatParseError("No <p> paragraphs found in TTML file.");

	std::stable_sort(paragraphs.begin(), paragraphs.end(),
		[](TtmlParagraph const& a, TtmlParagraph const& b) { return a.begin_ms < b.begin_ms; });

	for (auto const& para : paragraphs) {
		// The events list uses an auto-unlink intrusive hook and owns its
		// nodes via delete-on-dispose, so entries must be raw new'd like
		// every other reader does; a smart pointer would unlink and free
		// each row the moment it goes out of scope.
		auto diag = new AssDialogue;
		diag->Start = agi::Time(std::max<int64_t>(para.begin_ms, 0));
		diag->End = agi::Time(std::max(para.end_ms, para.begin_ms));
		diag->Text = para.karaoke_text;
		target->Events.push_back(*diag);
	}
}
