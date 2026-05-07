#include "subtitle_overflow.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "async_video_provider.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "project.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

#include <wx/bitmap.h>
#include <wx/dcmemory.h>
#include <wx/font.h>

namespace {

struct TextState {
	std::string font;
	double fontsize = 48.;
	double scalex = 100.;
	double scaley = 100.;
	double spacing = 0.;
	double script_to_video_x = 1.;
	double script_to_video_y = 1.;
	bool bold = false;
	bool italic = false;
	int drawing = 0;
};

size_t utf8_char_len(unsigned char c) {
	if (c < 0x80) return 1;
	if ((c & 0xE0) == 0xC0) return 2;
	if ((c & 0xF0) == 0xE0) return 3;
	if ((c & 0xF8) == 0xF0) return 4;
	return 1;
}

void skip_spaces(std::string_view text, size_t& pos) {
	while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
		++pos;
}

double parse_number(std::string_view text, size_t& pos, double fallback) {
	skip_spaces(text, pos);
	const char *start = text.data() + pos;
	char *end = nullptr;
	double value = std::strtod(start, &end);
	if (end == start)
		return fallback;
	pos = static_cast<size_t>(end - text.data());
	return value;
}

int parse_boolish(std::string_view text, size_t& pos, bool fallback) {
	skip_spaces(text, pos);
	if (pos >= text.size() || text[pos] == '\\')
		return fallback;
	if (text[pos] == '0' || text[pos] == '1') {
		int value = text[pos] == '1';
		++pos;
		return value;
	}
	return parse_number(text, pos, fallback) != 0.;
}

bool starts_with(std::string_view text, size_t pos, std::string_view token) {
	return pos + token.size() <= text.size() && text.substr(pos, token.size()) == token;
}

TextState base_state(AssStyle const& style) {
	TextState state;
	state.font = style.font;
	state.fontsize = style.fontsize;
	state.scalex = style.scalex;
	state.scaley = style.scaley;
	state.spacing = style.spacing;
	state.bold = style.bold;
	state.italic = style.italic;
	return state;
}

void apply_override_block(std::string_view text, TextState& state, TextState const& base, AssFile const& ass) {
	for (size_t pos = 0; pos < text.size(); ++pos) {
		if (text[pos] != '\\')
			continue;
		++pos;

		if (starts_with(text, pos, "fn")) {
			pos += 2;
			size_t start = pos;
			while (pos < text.size() && text[pos] != '\\')
				++pos;
			state.font = std::string(text.substr(start, pos - start));
			--pos;
		}
		else if (starts_with(text, pos, "fs")) {
			pos += 2;
			if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
				char sign = text[pos++];
				double value = parse_number(text, pos, 0.);
				state.fontsize = std::max(1., state.fontsize + (sign == '-' ? -value : value));
			}
			else {
				state.fontsize = std::max(1., parse_number(text, pos, state.fontsize));
			}
			--pos;
		}
		else if (starts_with(text, pos, "fscx")) {
			pos += 4;
			state.scalex = parse_number(text, pos, state.scalex);
			--pos;
		}
		else if (starts_with(text, pos, "fscy")) {
			pos += 4;
			state.scaley = parse_number(text, pos, state.scaley);
			--pos;
		}
		else if (starts_with(text, pos, "fsp")) {
			pos += 3;
			state.spacing = parse_number(text, pos, state.spacing);
			--pos;
		}
		else if (text[pos] == 'b') {
			++pos;
			state.bold = parse_boolish(text, pos, base.bold);
			--pos;
		}
		else if (text[pos] == 'i') {
			++pos;
			state.italic = parse_boolish(text, pos, base.italic);
			--pos;
		}
		else if (text[pos] == 'p') {
			++pos;
			state.drawing = std::max(0, static_cast<int>(parse_number(text, pos, state.drawing)));
			--pos;
		}
		else if (text[pos] == 'r') {
			++pos;
			size_t start = pos;
			while (pos < text.size() && text[pos] != '\\')
				++pos;
			std::string style_name(text.substr(start, pos - start));
			if (style_name.empty()) {
				state = base;
			}
			else if (AssStyle *style = const_cast<AssFile&>(ass).GetStyle(style_name)) {
				state = base_state(*style);
				state.script_to_video_x = base.script_to_video_x;
				state.script_to_video_y = base.script_to_video_y;
			}
			else {
				state = base;
			}
			--pos;
		}
	}
}

wxFont make_font(TextState const& state) {
	wxFont font = *wxNORMAL_FONT;
	font.SetEncoding(wxFONTENCODING_DEFAULT);
	font.SetPixelSize(wxSize(0, static_cast<int>(std::max(1., state.fontsize * state.script_to_video_y * state.scaley / 100.))));
	if (!state.font.empty())
		font.SetFaceName(to_wx(state.font));
	font.SetWeight(state.bold ? wxFONTWEIGHT_BOLD : wxFONTWEIGHT_NORMAL);
	font.SetStyle(state.italic ? wxFONTSTYLE_ITALIC : wxFONTSTYLE_NORMAL);
	return font;
}

int measure_char(wxDC& dc, TextState const& state, std::string_view raw) {
	dc.SetFont(make_font(state));
	wxSize extent = dc.GetTextExtent(to_wx(raw));
	return static_cast<int>(extent.GetWidth() * state.scalex / 100.);
}

int line_margin(AssDialogue const& line, AssStyle const& style, int index) {
	return line.Margin[index] ? line.Margin[index] : style.Margin[index];
}

subtitle_overflow::Result check_with_dc(agi::Context *context, AssDialogue const *line, wxDC& dc) {
	subtitle_overflow::Result result;

	if (!context || !line || line->Comment || !OPT_GET("Subtitle/Overflow Highlight/Enabled")->GetBool())
		return result;

	auto provider = context->project->VideoProvider();
	if (!provider)
		return result;

	int script_w = 0;
	int script_h = 0;
	context->ass->GetResolution(script_w, script_h);
	if (script_w <= 0 || script_h <= 0)
		return result;

	int video_w = provider->GetWidth();
	int video_h = provider->GetHeight();
	if (video_w <= 0 || video_h <= 0)
		return result;

	result.valid = true;

	AssStyle default_style;
	AssStyle const *style = context->ass->GetStyle(line->Style);
	if (!style)
		style = &default_style;

	const double scale_x = static_cast<double>(video_w) / script_w;
	const double scale_y = static_cast<double>(video_h) / script_h;
	const int available_width = std::max(1, static_cast<int>(video_w - (line_margin(*line, *style, 0) + line_margin(*line, *style, 1)) * scale_x));
	TextState base = base_state(*style);
	base.script_to_video_x = scale_x;
	base.script_to_video_y = scale_y;
	TextState state = base;

	wxFont old_font = dc.GetFont();
	auto const& text = line->Text.get();
	int width = 0;
	bool first_char = true;
	bool segment_overflow = false;

	auto reset_segment = [&] {
		width = 0;
		first_char = true;
		segment_overflow = false;
	};

	for (size_t pos = 0; pos < text.size();) {
		if (text[pos] == '{') {
			size_t end = text.find('}', pos + 1);
			if (end == std::string::npos)
				break;
			apply_override_block(std::string_view(text).substr(pos + 1, end - pos - 1), state, base, *context->ass);
			pos = end + 1;
			continue;
		}

		if (text[pos] == '\\' && pos + 1 < text.size()) {
			char command = text[pos + 1];
			if (command == 'N' || command == 'n') {
				reset_segment();
				pos += 2;
				continue;
			}
			if (command == 'h') {
				size_t start = pos;
				pos += 2;
				if (state.drawing > 0)
					continue;

				int char_width = measure_char(dc, state, " ");
				int next_width = width + (first_char ? 0 : static_cast<int>(state.spacing * state.script_to_video_x)) + char_width;
				if (next_width > available_width || segment_overflow) {
					result.overflow = true;
					segment_overflow = true;
					result.ranges.push_back({ static_cast<int>(start), 2 });
				}
				width = next_width;
				first_char = false;
				continue;
			}
		}

		size_t start = pos;
		size_t length = utf8_char_len(static_cast<unsigned char>(text[pos]));
		pos = std::min(text.size(), pos + length);
		if (state.drawing > 0)
			continue;

		int char_width = measure_char(dc, state, std::string_view(text).substr(start, pos - start));
		int next_width = width + (first_char ? 0 : static_cast<int>(state.spacing * state.script_to_video_x)) + char_width;
		if (next_width > available_width || segment_overflow) {
			result.overflow = true;
			segment_overflow = true;
			result.ranges.push_back({ static_cast<int>(start), static_cast<int>(pos - start) });
		}
		width = next_width;
		first_char = false;
	}

	dc.SetFont(old_font);
	return result;
}

}

namespace subtitle_overflow {

Result Check(agi::Context *context, AssDialogue const *line, wxDC *dc) {
	if (dc)
		return check_with_dc(context, line, *dc);

	wxBitmap bmp(1, 1);
	wxMemoryDC mem_dc;
	mem_dc.SelectObject(bmp);
	auto result = check_with_dc(context, line, mem_dc);
	mem_dc.SelectObject(wxNullBitmap);
	return result;
}

}
