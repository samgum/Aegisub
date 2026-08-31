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
/// maps to ASS \k karaoke durations; <br/> becomes \N. Namespaced documents
/// (any prefix, or a default namespace) are handled by comparing local names.
/// @ingroup subtitle_io

#include "subtitle_format_ttml.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "compat.h"
#include "options.h"

#include <libaegisub/ass/time.h>

#include <wx/xml/xml.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {
// Parse a TTML clock-time or offset-time value to milliseconds.
// Supports "HH:MM:SS.mmm", "MM:SS.mmm", "SS.mmm", "123ms", "4.5s", "2m", "1h".
int64_t ParseTTMLTime(std::string_view value) {
	boost::trim(value);
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
		if (std::from_chars(num.data(), num.data() + num.size(), amount).ec != std::errc{})
			return -1;
		if (suffix == "ms") return static_cast<int64_t>(std::llround(amount));
		if (suffix == "s") return static_cast<int64_t>(std::llround(amount * 1000));
		if (suffix == "m") return static_cast<int64_t>(std::llround(amount * 60000));
		if (suffix == "h") return static_cast<int64_t>(std::llround(amount * 3600000));
		return -1;
	}

	// Clock time with colons. Fraction is separated by '.' (or ',' per spec).
	size_t first_colon = value.find(':');
	if (first_colon == std::string::npos) {
		// Bare seconds like "12.5"
		double seconds = 0.0;
		if (std::from_chars(value.data(), value.data() + value.size(), seconds).ec != std::errc{})
			return -1;
		return static_cast<int64_t>(std::llround(seconds * 1000));
	}

	auto parse_component = [](std::string_view str, double& out) -> bool {
		if (!str.empty() && str.front() == ',') str.remove_prefix(1);
		if (str.empty()) return false;
		return std::from_chars(str.data(), str.data() + str.size(), out).ec == std::errc{};
	};

	// Optional leading fraction on the seconds part.
	double seconds = 0.0;
	size_t sec_start = value.find_last_of(':');
	std::string_view sec_part = value.substr(sec_start + 1);
	size_t dot = sec_part.find_first_of(".,");
	std::string_view frac;
	if (dot != std::string::npos) {
		frac = sec_part.substr(dot);
		sec_part = sec_part.substr(0, dot);
	}
	if (!parse_component(sec_part, seconds)) return -1;

	int64_t hours = 0, minutes = 0;
	if (sec_start > 0) {
		std::string_view mm_part = value.substr(0, sec_start);
		size_t mm_colon = mm_part.find_last_of(':');
		std::string_view mm_sv = mm_colon == std::string::npos ? mm_part : mm_part.substr(mm_colon + 1);
		double mm = 0.0;
		if (!parse_component(mm_sv, mm)) return -1;
		minutes = static_cast<int64_t>(mm);

		if (mm_colon != std::string::npos) {
			double hh = 0.0;
			if (!parse_component(mm_part.substr(0, mm_colon), hh)) return -1;
			hours = static_cast<int64_t>(hh);
		}
	}

	double frac_seconds = 0.0;
	if (!frac.empty()) {
		// frac includes its separator ('.' or ',') and possibly more colons
		// (frame-based "begin" values are not supported; treat '.' only).
		if (frac.front() == '.' || frac.front() == ',') {
			frac.remove_prefix(1);
			if (!frac.empty() && frac.find(':') == std::string::npos) {
				std::string_view digits = frac;
				// "5" = .5 s, "50" = .50 s, "500" = .5 s — spec says fraction of second
				double v = 0.0;
				if (std::from_chars(digits.data(), digits.data() + digits.size(), v).ec == std::errc{})
					frac_seconds = v / std::pow(10.0, static_cast<double>(digits.size()));
			}
		}
	}

	return ((hours * 60 + minutes) * 60 + static_cast<int64_t>(seconds)) * 1000
		+ static_cast<int64_t>(std::llround(frac_seconds * 1000));
}

// Local name of an XML node with any namespace prefix stripped.
std::string LocalName(wxXmlNode const* node) {
	return node->GetName().ToStdString(); // wxXml already reports unprefixed names for parsed docs
}

bool IsElement(wxXmlNode const* node, std::string_view local) {
	return LocalName(node) == local;
}

struct TtmlParagraph {
	int64_t begin_ms = 0;
	int64_t end_ms = 0;
	std::string karaoke_text; // ASS text with \k segments when timed spans exist
	bool has_karaoke = false;
};

// Determine (begin, end) for a paragraph from begin/end/dur attributes.
bool ParseParagraphTimes(wxXmlNode const* p, int64_t& begin_ms, int64_t& end_ms) {
	std::string begin_str, end_str, dur_str;
	for (auto attr = p->GetAttributes(); attr; attr = attr->GetNext()) {
		std::string name = attr->GetName().ToStdString();
		auto pos = name.find(':');
		if (pos != std::string::npos) name = name.substr(pos + 1);
		if (name == "begin") begin_str = attr->GetValue().ToStdString();
		else if (name == "end") end_str = attr->GetValue().ToStdString();
		else if (name == "dur") dur_str = attr->GetValue().ToStdString();
	}

	begin_ms = ParseTTMLTime(begin_str);
	if (begin_ms < 0) return false;

	if (!end_str.empty()) {
		end_ms = ParseTTMLTime(end_str);
		if (end_ms < 0) end_ms = begin_ms;
	}
	else if (!dur_str.empty()) {
		int64_t dur = ParseTTMLTime(dur_str);
		end_ms = dur < 0 ? begin_ms : begin_ms + dur;
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
void BuildParagraphText(wxXmlNode *p, int64_t begin_ms, int64_t end_ms,
	std::string& plain, bool& has_karaoke,
	std::vector<std::pair<int64_t, std::string>>& segments) {
	for (auto node = p->GetChildren(); node; node = node->GetNext()) {
		switch (node->GetType()) {
			case wxXML_TEXT_NODE:
			case wxXML_CDATA_SECTION_NODE: {
				std::string text = node->GetContent().ToStdString();
				if (has_karaoke && !segments.empty())
					segments.back().second += text;
				else
					plain += text;
				break;
			}
			case wxXML_ELEMENT_NODE: {
				if (IsElement(node, "br")) {
					if (has_karaoke && !segments.empty())
						segments.back().second += "\\N";
					else
						plain += "\\N";
					break;
				}
				if (IsElement(node, "span")) {
					std::string begin_attr;
					for (auto attr = node->GetAttributes(); attr; attr = attr->GetNext()) {
						std::string name = attr->GetName().ToStdString();
						auto pos = name.find(':');
						if (pos != std::string::npos) name = name.substr(pos + 1);
						if (name == "begin") begin_attr = attr->GetValue().ToStdString();
					}

					// Collect this span's own text (and nested spans flatten).
					std::string span_text;
					for (auto child = node->GetChildren(); child; child = child->GetNext()) {
						if (child->GetType() == wxXML_TEXT_NODE || child->GetType() == wxXML_CDATA_SECTION_NODE)
							span_text += child->GetContent().ToStdString();
						else if (IsElement(child, "br"))
							span_text += "\\N";
					}

					int64_t span_begin = ParseTTMLTime(begin_attr);
					if (span_begin >= 0 && !span_text.empty()) {
						has_karaoke = true;
						segments.emplace_back(span_begin, span_text);
					}
					else if (!span_text.empty()) {
						// Untimed span: append to the plain text path.
						plain += span_text;
					}
					break;
				}
				// Other elements: recurse so metadata wrappers never eat text.
				BuildParagraphText(node, begin_ms, end_ms, plain, has_karaoke, segments);
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
	if (!doc.Load(filename.wstring())) throw SubtitleFormatParseError("Failed loading TTML XML file.");
	if (!doc.GetRoot() || !IsElement(doc.GetRoot(), "tt"))
		throw SubtitleFormatParseError("Invalid TTML file: root element is not <tt>.");

	std::vector<TtmlParagraph> paragraphs;

	// Depth-first search for <p> elements anywhere below the root (they live
	// under body/div in valid TTML, but real-world files vary).
	for (auto node = doc.GetRoot(); node; node = node->GetNext()) {
		if (node->GetType() != wxXML_ELEMENT_NODE) continue;
		if (IsElement(node, "p")) {
			// unreachable: root itself is <tt>; kept for symmetry
		}

		// Iterative traversal from this top-level node.
		std::vector<wxXmlNode*> stack{node};
		while (!stack.empty()) {
			wxXmlNode *cur = stack.back();
			stack.pop_back();
			if (cur->GetType() != wxXML_ELEMENT_NODE) continue;

			if (IsElement(cur, "p")) {
				TtmlParagraph para;
				if (!ParseParagraphTimes(cur, para.begin_ms, para.end_ms)) continue;

				std::string plain;
				BuildParagraphText(cur, para.begin_ms, para.end_ms, plain, para.has_karaoke, para.segments);

				boost::trim(plain);
				if (para.has_karaoke) {
					// Plain text seen before the first timed span (rare) is
					// kept as a leading untimed run.
					if (!plain.empty())
						para.karaoke_text = plain;
					std::stable_sort(para.segments.begin(), para.segments.end(),
						[](auto const& a, auto const& b) { return a.first < b.first; });
					auto karaoke_cs = [](int64_t ms) { return static_cast<int>((ms + 5) / 10); };
					for (size_t s = 0; s < para.segments.size(); ++s) {
						int64_t seg_end = s + 1 < para.segments.size()
							? para.segments[s + 1].first
							: std::max(para.end_ms, para.segments[s].first + 500);
						int64_t dur = std::max<int64_t>(seg_end - para.segments[s].first, 0);
						para.karaoke_text += "{\\k" + std::to_string(karaoke_cs(dur)) + "}" + para.segments[s].second;
					}
				}
				else {
					para.karaoke_text = plain;
				}

				boost::trim(para.karaoke_text);
				if (para.karaoke_text.empty()) continue;
				paragraphs.push_back(std::move(para));
				continue; // do not recurse into a <p>
			}

			for (auto child = cur->GetChildren(); child; child = child->GetNext())
				stack.push_back(child);
		}
	}

	if (paragraphs.empty())
		throw SubtitleFormatParseError("No <p> paragraphs found in TTML file.");

	std::stable_sort(paragraphs.begin(), paragraphs.end(),
		[](TtmlParagraph const& a, TtmlParagraph const& b) { return a.begin_ms < b.begin_ms; });

	for (auto const& para : paragraphs) {
		AssDialogue diag;
		diag.Start = agi::Time(std::max<int64_t>(para.begin_ms, 0));
		diag.End = agi::Time(std::max(para.end_ms, para.begin_ms));
		diag.Text = para.karaoke_text;
		target->Events.push_back(std::move(diag));
	}
}
