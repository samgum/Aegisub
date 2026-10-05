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

/// @file subtitle_format_lrc.cpp
/// @brief Reading LRC lyrics, including the enhanced (syllable-level, "A2")
/// variant with inline <mm:ss.xx> word timestamps. Word timestamps map to
/// ASS \kf sweeping karaoke durations so the import is karaoke-ready, and a
/// trailing word timestamp (as exported by Apple Music) marks the line end.
/// @ingroup subtitle_io

#include "subtitle_format_lrc.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "options.h"
#include "text_file_reader.h"

#include <libaegisub/ass/time.h>
#include <libaegisub/log.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <boost/algorithm/string/trim.hpp>

namespace {
struct LrcLine {
	int64_t start_ms = 0;                    // line start time
	int64_t end_marker_ms = -1;              // explicit line end from a trailing word timestamp
	std::string text;                        // plain text (no syllable tags)
	std::vector<std::pair<int64_t, std::string>> syllables; // <time, text> when enhanced
	bool has_syllables = false;
};

// Parse "mm:ss", "mm:ss.xx" or "mm:ss.xxx" (leading [ already consumed).
// Returns milliseconds, or -1 if the tag is not a timestamp.
int64_t ParseLrcTimestamp(std::string const& body) {
	size_t colon = body.find(':');
	if (colon == std::string::npos) return -1;

	auto parse_integer = [](std::string_view value, int64_t& out) {
		if (value.empty() || value.find_first_not_of("0123456789") != std::string_view::npos)
			return false;
		auto result = std::from_chars(value.data(), value.data() + value.size(), out);
		return result.ec == std::errc{} && result.ptr == value.data() + value.size();
	};
	int64_t minutes = 0;
	if (!parse_integer(std::string_view(body).substr(0, colon), minutes)) return -1;

	size_t dot = body.find('.', colon);
	// A string_view's explicit length is never npos: that produces an
	// invalid pointer range for from_chars on ordinary [mm:ss] timestamps.
	size_t sec_end = dot == std::string::npos ? body.size() : dot;
	std::string_view sec_str(body.data() + colon + 1, sec_end - colon - 1);
	int64_t seconds = 0;
	if (!parse_integer(sec_str, seconds) || seconds >= 60) return -1;

	int64_t frac_ms = 0;
	if (dot != std::string::npos) {
		std::string_view frac_str(body.data() + dot + 1, body.size() - dot - 1);
		if (frac_str.empty() || frac_str.size() > 3) return -1;
		int64_t frac = 0;
		if (!parse_integer(frac_str, frac)) return -1;
		// 1 digit = tenths, 2 = centiseconds, 3 = milliseconds
		frac_ms = frac_str.size() == 1 ? frac * 100 : frac_str.size() == 2 ? frac * 10 : frac;
	}
	// agi::Time consumes int milliseconds. Bound input before arithmetic.
	if (minutes > (std::numeric_limits<int>::max() - seconds * 1000 - frac_ms) / 60000)
		return -1;

	return (minutes * 60 + seconds) * 1000 + frac_ms;
}

// Parse one bracketed tag body; returns true when it is a timestamp whose
// value is written to ms_out, false for metadata/other tags.
bool TryParseTag(std::string const& body, int64_t& ms_out) {
	int64_t ms = ParseLrcTimestamp(body);
	if (ms < 0) return false;
	ms_out = ms;
	return true;
}
}

LrcSubtitleFormat::LrcSubtitleFormat()
: SubtitleFormat("LRC Lyrics")
{
}

std::vector<std::string> LrcSubtitleFormat::GetReadWildcards() const {
	return {"lrc"};
}

void LrcSubtitleFormat::ReadFile(AssFile *target, agi::fs::path const& filename, agi::vfr::Framerate const&, const char *encoding) const {
	TextFileReader file(filename, encoding, false);
	target->LoadDefault(false, OPT_GET("Subtitle Format/TXT/Default Style Catalog")->GetString());

	int64_t offset_ms = 0;
	std::vector<LrcLine> lines;
	std::vector<std::string> untimed_lines;

	while (file.HasMoreLines()) {
		std::string line = file.ReadLineFromFile();
		boost::trim(line);
		if (line.empty()) continue;
		if (line[0] != '[') {
			// Some sources export plain lyrics text under an .lrc extension
			// with no timestamps at all; keep those lines as a fallback.
			untimed_lines.push_back(std::move(line));
			continue;
		}

		// Peel leading [tags] off the line.
		std::vector<int64_t> timestamps;
		std::vector<std::pair<int64_t, std::string>> syllables;
		bool has_syllables = false;
		int64_t end_marker_ms = -1;

		size_t pos = 0;
		while (pos < line.size() && line[pos] == '[') {
			size_t close = line.find(']', pos);
			if (close == std::string::npos) break;
			std::string body = line.substr(pos + 1, close - pos - 1);
			boost::trim(body);

			int64_t ms = 0;
			if (TryParseTag(body, ms)) {
				timestamps.push_back(ms);
			}
			else if (body.starts_with("offset:")) {
				int64_t off = 0;
				auto val = std::string_view(body).substr(7);
				// string_view has no erase(); strip spaces by moving the ends.
				while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
					val.remove_prefix(1);
				while (!val.empty() && (val.back() == ' ' || val.back() == '\t'))
					val.remove_suffix(1);
				if (!val.empty() && val.front() == '+') val.remove_prefix(1);
				if (!val.empty()) {
					auto parsed = std::from_chars(val.data(), val.data() + val.size(), off);
					if (parsed.ec == std::errc{} && parsed.ptr == val.data() + val.size()
						&& off >= -int64_t(std::numeric_limits<int>::max()) && off <= std::numeric_limits<int>::max())
						offset_ms = off; // applied to all rows after reading the file
				}
			}
			// other metadata tags (ti/ar/al/by/re/ve/...) are ignored

			pos = close + 1;
		}

		std::string text = line.substr(pos);
		if (timestamps.empty()) continue;
		boost::trim(text);

		// Enhanced LRC: split <mm:ss.xx> syllable markers inside the text.
		size_t scan = 0;
		int64_t last_syllable_ms = timestamps.front();
		std::string pending_text;
		while (true) {
			size_t lt = text.find('<', scan);
			size_t gt = text.find('>', lt == std::string::npos ? std::string::npos : lt + 1);
			if (lt == std::string::npos || gt == std::string::npos) {
				pending_text += text.substr(scan);
				break;
			}
			std::string tag_body = text.substr(lt + 1, gt - lt - 1);
			int64_t ms = 0;
			// Enhanced-LRC syllable tags use the same timestamp syntax.
			if (!TryParseTag(tag_body, ms)) {
				// Not a syllable timestamp; keep it verbatim (could be an
				// ASS-injected tag someone left in) and move past this '<'.
				pending_text += text.substr(scan, gt - scan + 1);
				scan = gt + 1;
				continue;
			}

			pending_text += text.substr(scan, lt - scan);
			if (!pending_text.empty()) {
				syllables.emplace_back(last_syllable_ms, pending_text);
				pending_text.clear();
			}
			last_syllable_ms = ms;
			has_syllables = true;
			scan = gt + 1;
		}
		boost::trim(pending_text);
		if (has_syllables && !pending_text.empty())
			syllables.emplace_back(last_syllable_ms, pending_text);
		else if (has_syllables)
			// A word timestamp with no text after it (the trailing
			// "<mm:ss.xx>" Apple Music exports) marks where the line ends,
			// so the final word must not stretch to the next line's start.
			end_marker_ms = last_syllable_ms;

		for (auto ts : timestamps) {
			LrcLine out;
			out.start_ms = ts;
			out.end_marker_ms = end_marker_ms < 0 ? -1 : end_marker_ms + ts - timestamps.front();
			out.has_syllables = has_syllables;
			if (has_syllables) {
				out.syllables = syllables;
				for (auto& syl : out.syllables)
					syl.first += ts - timestamps.front();
			}
			else {
				out.text = text;
				out.text.erase(std::remove(out.text.begin(), out.text.end(), '\r'), out.text.end());
			}
			// Timestamp-only lines (e.g. "[00:23.05]" with no text after it,
			// common in Apple Music exports as section spacers) and lines
			// whose word markers carry no text would become empty dialogue
			// rows; skip them.
			bool empty_line = (!out.has_syllables || out.syllables.empty())
				&& out.text.find_first_not_of(" \t") == std::string::npos;
			if (!empty_line)
				lines.push_back(std::move(out));
		}
	}

	if (lines.empty() && !untimed_lines.empty()) {
		// Untimed LRC: import like the plain-text reader does, one untimed
		// row per lyric line, for the user to time.
		for (std::string& text : untimed_lines) {
			auto diag = new AssDialogue;
			diag->Text = std::move(text);
			target->Events.push_back(*diag);
		}
		return;
	}

	if (lines.empty())
		throw SubtitleFormatParseError("No timed lyrics lines found in LRC file.");

	// offset is file-wide metadata and may appear after the lyric rows.
	auto adjusted_time = [&](int64_t time) {
		return std::clamp<int64_t>(time - offset_ms, 0, std::numeric_limits<int>::max() - 5000);
	};
	for (auto& line : lines) {
		line.start_ms = adjusted_time(line.start_ms);
		if (line.end_marker_ms >= 0) line.end_marker_ms = adjusted_time(line.end_marker_ms);
		for (auto& syl : line.syllables) syl.first = adjusted_time(syl.first);
	}

	// Sort by start time; LRC files are usually ordered but multi-timestamp
	// expansion can interleave.
	std::stable_sort(lines.begin(), lines.end(),
		[](LrcLine const& a, LrcLine const& b) { return a.start_ms < b.start_ms; });

	// Next-row starts and explicit end markers are authoritative, even
	// for words shorter than 500 ms. Only missing/invalid ends need a fallback.
	auto karaoke_cs = [](int64_t ms) { return static_cast<int>((ms + 5) / 10); };

	for (size_t i = 0; i < lines.size(); ++i) {
		auto const& cur = lines[i];
		int64_t end_ms = i + 1 < lines.size() ? lines[i + 1].start_ms : cur.start_ms + 5000;
		if (cur.end_marker_ms >= 0)
			end_ms = cur.end_marker_ms;
		if (i + 1 < lines.size() && end_ms > lines[i + 1].start_ms)
			end_ms = lines[i + 1].start_ms;
		if (end_ms <= cur.start_ms) end_ms = cur.start_ms + 500;

		// The events list uses an auto-unlink intrusive hook and owns its
		// nodes via delete-on-dispose, so entries must be raw new'd like
		// every other reader does; a smart pointer would unlink and free
		// each row the moment it goes out of scope.
		auto diag = new AssDialogue;
		diag->Start = agi::Time(cur.start_ms);
		diag->End = agi::Time(end_ms);

		if (cur.has_syllables && !cur.syllables.empty()) {
			// \kf takes centiseconds of *duration per segment* and sweeps the
			// highlight across each word for that duration (the Apple Music
			// word-fill look); the final segment runs to the line end.
			std::string text;
			int64_t elapsed_cs = 0;
			for (size_t s = 0; s < cur.syllables.size(); ++s) {
				auto const& syl = cur.syllables[s];
				int64_t seg_begin = std::clamp(syl.first, cur.start_ms, end_ms);
				int64_t begin_cs = karaoke_cs(seg_begin - cur.start_ms);
				if (begin_cs > elapsed_cs) {
					text += "{\\k" + std::to_string(begin_cs - elapsed_cs) + "}";
					elapsed_cs = begin_cs;
				}
				int64_t seg_end = s + 1 < cur.syllables.size() ? cur.syllables[s + 1].first : end_ms;
				int64_t end_cs = karaoke_cs(std::clamp(seg_end, seg_begin, end_ms) - cur.start_ms);
				int64_t dur_cs = std::max<int64_t>(end_cs - elapsed_cs, 0);
				text += "{\\kf" + std::to_string(dur_cs) + "}" + syl.second;
				elapsed_cs += dur_cs;
			}
			diag->Text = text;
		}
		else {
			diag->Text = cur.text;
		}

		target->Events.push_back(*diag);
	}
}
