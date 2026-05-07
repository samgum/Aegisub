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
#include <vector>

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
	double outline_w = 0.;
	double shadow_w = 0.;
	double script_to_video_x = 1.;
	double script_to_video_y = 1.;
	bool bold = false;
	bool italic = false;
	int drawing = 0;
};

struct LayoutState {
	int alignment = 2;
	bool positioned = false;
	double x = 0.;
	double x2 = 0.;
	bool has_secondary_x = false;
};

struct MeasuredChar {
	int start = 0;
	int length = 0;
	double left = 0.;
	double right = 0.;
	double pad_left = 0.;
	double pad_right = 0.;
};

struct MeasuredSegment {
	std::vector<MeasuredChar> chars;
	double width = 0.;
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

void skip_arg_separator(std::string_view text, size_t& pos) {
	skip_spaces(text, pos);
	if (pos < text.size() && text[pos] == ',')
		++pos;
	skip_spaces(text, pos);
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

bool parse_tag_numbers(std::string_view text, size_t& pos, double *values, size_t count) {
	skip_spaces(text, pos);
	if (pos >= text.size() || text[pos] != '(')
		return false;
	++pos;

	for (size_t i = 0; i < count; ++i) {
		skip_arg_separator(text, pos);
		if (pos >= text.size() || text[pos] == ')' || text[pos] == '\\')
			return false;

		size_t before = pos;
		values[i] = parse_number(text, pos, values[i]);
		if (pos == before)
			return false;
	}

	return true;
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
	state.outline_w = style.outline_w;
	state.shadow_w = style.shadow_w;
	state.bold = style.bold;
	state.italic = style.italic;
	return state;
}

void apply_override_block(std::string_view text, TextState& state, TextState const& base, LayoutState& layout, AssFile const& ass) {
	for (size_t pos = 0; pos < text.size(); ++pos) {
		if (text[pos] != '\\')
			continue;
		++pos;

		if (starts_with(text, pos, "pos")) {
			pos += 3;
			double values[2] = { layout.x, 0. };
			if (parse_tag_numbers(text, pos, values, 2)) {
				layout.positioned = true;
				layout.x = values[0];
				layout.x2 = values[0];
				layout.has_secondary_x = false;
			}
			--pos;
		}
		else if (starts_with(text, pos, "move")) {
			pos += 4;
			double values[4] = { layout.x, 0., layout.x, 0. };
			if (parse_tag_numbers(text, pos, values, 4)) {
				layout.positioned = true;
				layout.x = values[0];
				layout.x2 = values[2];
				layout.has_secondary_x = true;
			}
			--pos;
		}
		else if (starts_with(text, pos, "an")) {
			pos += 2;
			int align = static_cast<int>(parse_number(text, pos, layout.alignment));
			if (align >= 1 && align <= 9)
				layout.alignment = align;
			--pos;
		}
		else if (starts_with(text, pos, "fn")) {
			pos += 2;
			size_t start = pos;
			while (pos < text.size() && text[pos] != '\\')
				++pos;
			state.font = std::string(text.substr(start, pos - start));
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
		else if (starts_with(text, pos, "fsp")) {
			pos += 3;
			state.spacing = parse_number(text, pos, state.spacing);
			--pos;
		}
		else if (starts_with(text, pos, "xbord")) {
			pos += 5;
			state.outline_w = std::max(0., parse_number(text, pos, state.outline_w));
			--pos;
		}
		else if (starts_with(text, pos, "bord")) {
			pos += 4;
			state.outline_w = std::max(0., parse_number(text, pos, state.outline_w));
			--pos;
		}
		else if (starts_with(text, pos, "xshad")) {
			pos += 5;
			state.shadow_w = parse_number(text, pos, state.shadow_w);
			--pos;
		}
		else if (starts_with(text, pos, "shad")) {
			pos += 4;
			state.shadow_w = parse_number(text, pos, state.shadow_w);
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
		else if (text[pos] == 'a') {
			++pos;
			int align = AssStyle::SsaToAss(static_cast<int>(parse_number(text, pos, AssStyle::AssToSsa(layout.alignment))));
			if (align >= 1 && align <= 9)
				layout.alignment = align;
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

double measure_char(wxDC& dc, TextState const& state, std::string_view raw) {
	dc.SetFont(make_font(state));
	wxSize extent = dc.GetTextExtent(to_wx(raw));
	return extent.GetWidth() * state.scalex / 100.;
}

double measure_text(wxDC& dc, TextState const& state, std::string_view raw) {
	if (raw.empty())
		return 0.;

	dc.SetFont(make_font(state));
	wxSize extent = dc.GetTextExtent(to_wx(raw));
	return extent.GetWidth() * state.scalex / 100.;
}

int line_margin(AssDialogue const& line, AssStyle const& style, int index) {
	return line.Margin[index] ? line.Margin[index] : style.Margin[index];
}

double line_anchor_x(LayoutState const& layout, AssDialogue const& line, AssStyle const& style, int script_w, double scale_x, bool secondary = false) {
	if (layout.positioned)
		return (secondary && layout.has_secondary_x ? layout.x2 : layout.x) * scale_x;

	int margin_l = line_margin(line, style, 0);
	int margin_r = line_margin(line, style, 1);
	int hor = (layout.alignment - 1) % 3;

	if (hor == 0)
		return margin_l * scale_x;
	if (hor == 1)
		return (script_w + margin_l - margin_r) * scale_x / 2.;
	return (script_w - margin_r) * scale_x;
}

double segment_left(double anchor_x, int alignment, double width) {
	int hor = (alignment - 1) % 3;
	if (hor == 0)
		return anchor_x;
	if (hor == 1)
		return anchor_x - width / 2.;
	return anchor_x - width;
}

void add_range(subtitle_overflow::Result& result, int start, int length) {
	if (length <= 0)
		return;

	if (!result.ranges.empty()) {
		auto& prev = result.ranges.back();
		int prev_end = prev.start + prev.length;
		if (start <= prev_end) {
			prev.length = std::max(prev_end, start + length) - prev.start;
			return;
		}
	}

	result.ranges.push_back({ start, length });
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
	TextState base = base_state(*style);
	base.script_to_video_x = scale_x;
	base.script_to_video_y = scale_y;
	TextState state = base;
	LayoutState layout;
	layout.alignment = style->alignment;

	wxFont old_font = dc.GetFont();
	auto const& text = line->Text.get();
	for (size_t pos = 0; pos < text.size();) {
		if (text[pos] != '{') {
			++pos;
			continue;
		}
		size_t end = text.find('}', pos + 1);
		if (end == std::string::npos)
			break;
		TextState scan_state = base;
		apply_override_block(std::string_view(text).substr(pos + 1, end - pos - 1), scan_state, base, layout, *context->ass);
		pos = end + 1;
	}

	MeasuredSegment segment;
	bool first_char = true;

	auto finish_segment = [&] {
		if (segment.chars.empty()) {
			segment = MeasuredSegment();
			first_char = true;
			return;
		}

		std::vector<bool> highlight(segment.chars.size(), false);
		auto check_anchor = [&](double anchor_x) {
			double left = segment_left(anchor_x, layout.alignment, segment.width);
			double bound_left = left;
			double bound_right = left + segment.width;
			for (auto const& ch : segment.chars) {
				bound_left = std::min(bound_left, left + ch.left - ch.pad_left);
				bound_right = std::max(bound_right, left + ch.right + ch.pad_right);
			}
			if (bound_left >= 0. && bound_right <= video_w)
				return;

			result.overflow = true;
			for (size_t i = 0; i < segment.chars.size(); ++i) {
				auto const& ch = segment.chars[i];
				double ch_left = left + ch.left - ch.pad_left;
				double ch_right = left + ch.right + ch.pad_right;
				if (ch_left < 0. || ch_right > video_w)
					highlight[i] = true;
			}
		};

		check_anchor(line_anchor_x(layout, *line, *style, script_w, scale_x));
		if (layout.has_secondary_x && layout.x2 != layout.x)
			check_anchor(line_anchor_x(layout, *line, *style, script_w, scale_x, true));

		for (size_t i = 0; i < segment.chars.size(); ++i) {
			if (highlight[i])
				add_range(result, segment.chars[i].start, segment.chars[i].length);
		}

		segment = MeasuredSegment();
		first_char = true;
	};

	auto leading_spacing = [&] {
		return first_char ? 0. : state.spacing * state.script_to_video_x * state.scalex / 100.;
	};

	auto add_plain_run = [&](size_t start, size_t end) {
		if (start >= end || state.drawing > 0)
			return;

		struct RunChar {
			size_t start = 0;
			size_t end = 0;
			double width = 0.;
		};
		std::vector<RunChar> chars;
		double summed_width = 0.;

		for (size_t pos = start; pos < end;) {
			size_t char_start = pos;
			pos = std::min(end, pos + utf8_char_len(static_cast<unsigned char>(text[pos])));

			double char_width = measure_char(dc, state, std::string_view(text).substr(char_start, pos - char_start));
			chars.push_back({ char_start, pos, char_width });
			summed_width += char_width;
		}

		if (chars.empty())
			return;

		double text_width = measure_text(dc, state, std::string_view(text).substr(start, end - start));
		double scale = summed_width > 0. ? text_width / summed_width : 1.;
		double spacing = state.spacing * state.script_to_video_x * state.scalex / 100.;
		double cursor = segment.width + leading_spacing();

		for (size_t i = 0; i < chars.size(); ++i) {
			if (i)
				cursor += spacing;
			double char_width = chars[i].width * scale;
			double char_left = cursor;
			double char_right = char_left + char_width;
			double outline = state.outline_w * state.script_to_video_x;
			double shadow = state.shadow_w * state.script_to_video_x;
			segment.chars.push_back({
				static_cast<int>(chars[i].start),
				static_cast<int>(chars[i].end - chars[i].start),
				char_left,
				char_right,
				outline + std::max(0., -shadow),
				outline + std::max(0., shadow)
			});
			cursor = char_right;
		}

		segment.width = cursor;
		first_char = false;
	};

	for (size_t pos = 0; pos < text.size();) {
		if (text[pos] == '{') {
			size_t end = text.find('}', pos + 1);
			if (end == std::string::npos)
				break;
			apply_override_block(std::string_view(text).substr(pos + 1, end - pos - 1), state, base, layout, *context->ass);
			pos = end + 1;
			continue;
		}

		if (text[pos] == '\\' && pos + 1 < text.size()) {
			char command = text[pos + 1];
			if (command == 'N' || command == 'n') {
				finish_segment();
				pos += 2;
				continue;
			}
			if (command == 'h') {
				size_t start = pos;
				pos += 2;
				if (state.drawing > 0)
					continue;

				double char_width = measure_char(dc, state, " ");
				double char_left = segment.width + leading_spacing();
				double char_right = char_left + char_width;
				double outline = state.outline_w * state.script_to_video_x;
				double shadow = state.shadow_w * state.script_to_video_x;
				segment.chars.push_back({
					static_cast<int>(start),
					2,
					char_left,
					char_right,
					outline + std::max(0., -shadow),
					outline + std::max(0., shadow)
				});
				segment.width = char_right;
				first_char = false;
				continue;
			}
		}

		size_t start = pos;
		while (pos < text.size()) {
			if (text[pos] == '{')
				break;
			if (text[pos] == '\\' && pos + 1 < text.size() && (text[pos + 1] == 'N' || text[pos + 1] == 'n' || text[pos + 1] == 'h'))
				break;
			pos = std::min(text.size(), pos + utf8_char_len(static_cast<unsigned char>(text[pos])));
		}
		add_plain_run(start, pos);
	}
	finish_segment();

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
