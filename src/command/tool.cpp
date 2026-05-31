// Copyright (c) 2005-2010, Niels Martin Hansen
// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// Copyright (c) 2010, Amar Takhar
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of the Aegisub Group nor the names of its contributors
//     may be used to endorse or promote products derived from this software
//     without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Aegisub Project http://www.aegisub.org/

#include "command.h"

#include "../ass_dialogue.h"
#include "../ass_file.h"
#include "../ass_style.h"
#include "../compat.h"
#include "../dialog_manager.h"
#include "../dialog_styling_assistant.h"
#include "../dialog_translation.h"
#include "../dialogs.h"
#include "../include/aegisub/context.h"
#include "../libresrc/libresrc.h"
#include "../options.h"
#include "../resolution_resampler.h"
#include "../selection_controller.h"
#include "../subs_preview.h"
#include "../video_controller.h"

#include <libaegisub/fs.h>
#include <libaegisub/path.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace {
	using cmd::Command;

const char *LYRIC_SCROLL_GENERATED = "MusicScroll Generated";
const char *LYRIC_SCROLL_SOURCE = "MusicScroll Source";

struct LyricScrollSettings {
	int scope = 0;
	int source_action = 1;
	bool clear_previous = true;
	bool strip_tags = true;
	bool preserve_line_breaks = true;
	bool animate = true;
	int resolution_preset = 0;
	int target_width = 3840;
	int target_height = 2160;
	int horizontal_alignment = 1;
	int center_x = 0;
	int center_y = 0;
	int line_gap = 0;
	int active_size = 0;
	int inactive_size = 0;
	int visible_lines = 3;
	int transition_ms = 700;
	int margin_lr = 0;
	int active_alpha = 0;
	int inactive_alpha = 88;
	int layer = 4;
	int wrap_after = 0;
	int outline_size = 0;
	int shadow_size = 0;
	std::string active_color = "&HFFFFFF&";
	std::string inactive_color = "&HD8D8D8&";
	std::string outline_color = "&H000000&";
};

struct ScrollResolution {
	int script_width = 3840;
	int script_height = 2160;
	int target_width = 3840;
	int target_height = 2160;
	double scale_x = 1.0;
	double scale_y = 1.0;
};

ScrollResolution resolve_scroll_resolution(AssFile *ass, LyricScrollSettings const& settings) {
	ScrollResolution res;
	ass->GetResolution(res.script_width, res.script_height);
	if (res.script_width <= 0) res.script_width = 3840;
	if (res.script_height <= 0) res.script_height = 2160;

	switch (settings.resolution_preset) {
		case 1:
			res.target_width = 3840;
			res.target_height = 2160;
			break;
		case 2:
			res.target_width = 1920;
			res.target_height = 1080;
			break;
		case 3:
			res.target_width = 1280;
			res.target_height = 720;
			break;
		case 4:
			res.target_width = 1080;
			res.target_height = 1920;
			break;
		case 5:
			res.target_width = 720;
			res.target_height = 1280;
			break;
		case 6:
			res.target_width = std::max(1, settings.target_width);
			res.target_height = std::max(1, settings.target_height);
			break;
		default:
			res.target_width = res.script_width;
			res.target_height = res.script_height;
			break;
	}

	res.scale_x = static_cast<double>(res.script_width) / res.target_width;
	res.scale_y = static_cast<double>(res.script_height) / res.target_height;
	return res;
}

LyricScrollSettings resolve_layout_settings(LyricScrollSettings settings, ScrollResolution const& res) {
	bool portrait = res.target_height > res.target_width;
	if (settings.margin_lr <= 0)
		settings.margin_lr = std::max(20, static_cast<int>(std::lround(portrait ? res.target_width * 0.08 : res.target_width * 555.0 / 3840.0)));
	if (settings.center_x <= 0) {
		int alignment = std::max(0, std::min(2, settings.horizontal_alignment));
		int safe_margin = std::max(0, std::min(settings.margin_lr, res.target_width));
		if (alignment == 0)
			settings.center_x = safe_margin;
		else if (alignment == 2)
			settings.center_x = res.target_width - safe_margin;
		else
			settings.center_x = portrait ? res.target_width / 2 : static_cast<int>(std::lround(res.target_width * 2600.0 / 3840.0));
	}
	if (settings.center_y <= 0)
		settings.center_y = portrait ? static_cast<int>(std::lround(res.target_height * 0.55)) : res.target_height / 2;
	if (settings.line_gap <= 0)
		settings.line_gap = std::max(24, static_cast<int>(std::lround(portrait ? res.target_height * 0.088 : res.target_height * 250.0 / 2160.0)));
	if (settings.active_size <= 0)
		settings.active_size = std::max(24, static_cast<int>(std::lround(portrait ? res.target_width * 0.072 : res.target_height * 165.0 / 2160.0)));
	if (settings.inactive_size <= 0)
		settings.inactive_size = std::max(18, static_cast<int>(std::lround(portrait ? res.target_width * 0.054 : res.target_height * 125.0 / 2160.0)));
	return settings;
}

void apply_lyric_scroll(agi::Context *c, LyricScrollSettings const& settings);
std::string plain_lyric_text(AssDialogue *line, LyricScrollSettings const& settings);
std::string escape_ass_text(std::string text);

int lyric_ass_alignment(LyricScrollSettings const& settings) {
	return 4 + std::max(0, std::min(2, settings.horizontal_alignment));
}

int opt_int(char const *name, int min_value, int max_value) {
	return std::max(min_value, std::min(max_value, static_cast<int>(OPT_GET(name)->GetInt())));
}

std::string trim_copy(std::string value) {
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
	value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), value.end());
	return value;
}

bool is_hex_string(std::string const& value) {
	return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

std::string normalize_ass_color(std::string value, char const *fallback) {
	value = trim_copy(value);
	if (value.size() == 7 && value[0] == '#') {
		std::string rgb = value.substr(1);
		if (is_hex_string(rgb))
			return "&H" + rgb.substr(4, 2) + rgb.substr(2, 2) + rgb.substr(0, 2) + "&";
	}
	if (value.size() == 6 && is_hex_string(value))
		return "&H" + value + "&";
	if (value.size() >= 4 && value[0] == '&' && (value[1] == 'H' || value[1] == 'h'))
		return value.back() == '&' ? value : value + "&";
	return fallback;
}

std::string opt_ass_color(char const *name, char const *fallback) {
	auto opt = OPT_GET(name);
	if (opt->GetType() == agi::OptionType::Color)
		return opt->GetColor().GetAssOverrideFormatted();
	return normalize_ass_color(opt->GetString(), fallback);
}

void set_opt_ass_color(char const *name, std::string const& value) {
	auto opt = OPT_SET(name);
	if (opt->GetType() == agi::OptionType::Color)
		opt->SetColor(agi::Color(value));
	else
		opt->SetString(value);
}

std::string remove_parenthesized_tag(std::string text, std::string const& tag) {
	size_t pos = 0;
	while ((pos = text.find(tag, pos)) != std::string::npos) {
		size_t open = text.find('(', pos + tag.size());
		if (open != pos + tag.size())
			break;

		int depth = 0;
		size_t end = open;
		for (; end < text.size(); ++end) {
			if (text[end] == '(')
				++depth;
			else if (text[end] == ')' && --depth == 0) {
				++end;
				break;
			}
		}
		text.erase(pos, end - pos);
	}
	return text;
}

std::string clean_motion_conflicts(std::string text) {
	text = remove_parenthesized_tag(std::move(text), "\\fad");
	text = remove_parenthesized_tag(std::move(text), "\\fade");
	text = remove_parenthesized_tag(std::move(text), "\\move");
	text = remove_parenthesized_tag(std::move(text), "\\pos");
	text = remove_parenthesized_tag(std::move(text), "\\org");
	return text;
}

size_t utf8_char_len(unsigned char c) {
	if ((c & 0x80) == 0) return 1;
	if ((c & 0xE0) == 0xC0) return 2;
	if ((c & 0xF0) == 0xE0) return 3;
	if ((c & 0xF8) == 0xF0) return 4;
	return 1;
}

std::string wrap_text(std::string text, int wrap_after) {
	if (wrap_after <= 0)
		return text;

	size_t line_start = 0;
	size_t last_space = std::string::npos;
	int chars = 0;

	for (size_t pos = 0; pos < text.size();) {
		if (text.compare(pos, 2, "\\N") == 0 || text[pos] == '\n') {
			pos += text[pos] == '\n' ? 1 : 2;
			line_start = pos;
			last_space = std::string::npos;
			chars = 0;
			continue;
		}

		size_t len = utf8_char_len(static_cast<unsigned char>(text[pos]));
		if (pos + len > text.size())
			len = 1;
		if (len == 1 && std::isspace(static_cast<unsigned char>(text[pos])))
			last_space = pos;

		++chars;
		if (chars >= wrap_after && pos + len < text.size()) {
			size_t break_pos = last_space != std::string::npos && last_space > line_start ? last_space : pos + len;
			text.replace(break_pos, last_space == break_pos ? 1 : 0, "\\N");
			pos = break_pos + 2;
			line_start = pos;
			last_space = std::string::npos;
			chars = 0;
			continue;
		}

		pos += len;
	}

	return text;
}

LyricScrollSettings load_lyric_scroll_settings() {
	LyricScrollSettings settings;
	settings.scope = opt_int("Tool/Lyric Scroll/Scope", 0, 1);
	settings.source_action = opt_int("Tool/Lyric Scroll/Source Action", 0, 2);
	settings.clear_previous = OPT_GET("Tool/Lyric Scroll/Clear Previous")->GetBool();
	settings.strip_tags = OPT_GET("Tool/Lyric Scroll/Strip Tags")->GetBool();
	settings.preserve_line_breaks = OPT_GET("Tool/Lyric Scroll/Preserve Line Breaks")->GetBool();
	settings.animate = OPT_GET("Tool/Lyric Scroll/Animate")->GetBool();
	settings.resolution_preset = opt_int("Tool/Lyric Scroll/Resolution Preset", 0, 6);
	settings.target_width = opt_int("Tool/Lyric Scroll/Target Width", 1, 10000);
	settings.target_height = opt_int("Tool/Lyric Scroll/Target Height", 1, 10000);
	settings.horizontal_alignment = opt_int("Tool/Lyric Scroll/Horizontal Alignment", 0, 2);
	settings.center_x = opt_int("Tool/Lyric Scroll/Center X", 0, 10000);
	settings.center_y = opt_int("Tool/Lyric Scroll/Center Y", 0, 10000);
	settings.line_gap = opt_int("Tool/Lyric Scroll/Line Gap", 0, 1000);
	settings.active_size = opt_int("Tool/Lyric Scroll/Active Size", 0, 400);
	settings.inactive_size = opt_int("Tool/Lyric Scroll/Inactive Size", 0, 400);
	settings.visible_lines = opt_int("Tool/Lyric Scroll/Visible Lines", 0, 8);
	settings.transition_ms = opt_int("Tool/Lyric Scroll/Transition MS", 0, 5000);
	settings.margin_lr = opt_int("Tool/Lyric Scroll/Margin LR", 0, 3000);
	settings.active_alpha = opt_int("Tool/Lyric Scroll/Active Alpha", 0, 255);
	settings.inactive_alpha = opt_int("Tool/Lyric Scroll/Inactive Alpha", 0, 255);
	settings.layer = opt_int("Tool/Lyric Scroll/Layer", 0, 999);
	settings.wrap_after = opt_int("Tool/Lyric Scroll/Wrap After", 0, 200);
	settings.outline_size = opt_int("Tool/Lyric Scroll/Outline Size", 0, 60);
	settings.shadow_size = opt_int("Tool/Lyric Scroll/Shadow Size", 0, 60);
	settings.active_color = opt_ass_color("Tool/Lyric Scroll/Active Color", "&HFFFFFF&");
	settings.inactive_color = opt_ass_color("Tool/Lyric Scroll/Inactive Color", "&HD8D8D8&");
	settings.outline_color = opt_ass_color("Tool/Lyric Scroll/Outline Color", "&H000000&");

	if (settings.center_x == 1920 && settings.center_y == 1120 && settings.line_gap == 150 &&
		settings.active_size == 88 && settings.inactive_size == 62 && settings.visible_lines == 2 &&
		settings.transition_ms == 360 && settings.margin_lr == 220 && settings.layer == 20 &&
		settings.wrap_after == 46 && settings.inactive_color == "&HA8A8A8&")
	{
		settings.center_x = 0;
		settings.center_y = 0;
		settings.line_gap = 0;
		settings.active_size = 0;
		settings.inactive_size = 0;
		settings.visible_lines = 3;
		settings.transition_ms = 700;
		settings.margin_lr = 0;
		settings.inactive_alpha = 88;
		settings.layer = 4;
		settings.wrap_after = 0;
		settings.inactive_color = "&HD8D8D8&";
	}

	return settings;
}

void save_lyric_scroll_settings(LyricScrollSettings const& settings) {
	OPT_SET("Tool/Lyric Scroll/Scope")->SetInt(settings.scope);
	OPT_SET("Tool/Lyric Scroll/Source Action")->SetInt(settings.source_action);
	OPT_SET("Tool/Lyric Scroll/Clear Previous")->SetBool(settings.clear_previous);
	OPT_SET("Tool/Lyric Scroll/Strip Tags")->SetBool(settings.strip_tags);
	OPT_SET("Tool/Lyric Scroll/Preserve Line Breaks")->SetBool(settings.preserve_line_breaks);
	OPT_SET("Tool/Lyric Scroll/Animate")->SetBool(settings.animate);
	OPT_SET("Tool/Lyric Scroll/Resolution Preset")->SetInt(settings.resolution_preset);
	OPT_SET("Tool/Lyric Scroll/Target Width")->SetInt(settings.target_width);
	OPT_SET("Tool/Lyric Scroll/Target Height")->SetInt(settings.target_height);
	OPT_SET("Tool/Lyric Scroll/Horizontal Alignment")->SetInt(settings.horizontal_alignment);
	OPT_SET("Tool/Lyric Scroll/Center X")->SetInt(settings.center_x);
	OPT_SET("Tool/Lyric Scroll/Center Y")->SetInt(settings.center_y);
	OPT_SET("Tool/Lyric Scroll/Line Gap")->SetInt(settings.line_gap);
	OPT_SET("Tool/Lyric Scroll/Active Size")->SetInt(settings.active_size);
	OPT_SET("Tool/Lyric Scroll/Inactive Size")->SetInt(settings.inactive_size);
	OPT_SET("Tool/Lyric Scroll/Visible Lines")->SetInt(settings.visible_lines);
	OPT_SET("Tool/Lyric Scroll/Transition MS")->SetInt(settings.transition_ms);
	OPT_SET("Tool/Lyric Scroll/Margin LR")->SetInt(settings.margin_lr);
	OPT_SET("Tool/Lyric Scroll/Active Alpha")->SetInt(settings.active_alpha);
	OPT_SET("Tool/Lyric Scroll/Inactive Alpha")->SetInt(settings.inactive_alpha);
	OPT_SET("Tool/Lyric Scroll/Layer")->SetInt(settings.layer);
	OPT_SET("Tool/Lyric Scroll/Wrap After")->SetInt(settings.wrap_after);
	OPT_SET("Tool/Lyric Scroll/Outline Size")->SetInt(settings.outline_size);
	OPT_SET("Tool/Lyric Scroll/Shadow Size")->SetInt(settings.shadow_size);
	set_opt_ass_color("Tool/Lyric Scroll/Active Color", settings.active_color);
	set_opt_ass_color("Tool/Lyric Scroll/Inactive Color", settings.inactive_color);
	set_opt_ass_color("Tool/Lyric Scroll/Outline Color", settings.outline_color);
}

class DialogLyricScroll final : public wxDialog {
	agi::Context *context = nullptr;
	wxRadioBox *scope = nullptr;
	wxChoice *source_action = nullptr;
	wxChoice *resolution_preset = nullptr;
	wxChoice *horizontal_alignment = nullptr;
	wxCheckBox *clear_previous = nullptr;
	wxCheckBox *strip_tags = nullptr;
	wxCheckBox *preserve_line_breaks = nullptr;
	wxCheckBox *animate = nullptr;
	wxSpinCtrl *target_width = nullptr;
	wxSpinCtrl *target_height = nullptr;
	wxSpinCtrl *center_x = nullptr;
	wxSpinCtrl *center_y = nullptr;
	wxSpinCtrl *line_gap = nullptr;
	wxSpinCtrl *active_size = nullptr;
	wxSpinCtrl *inactive_size = nullptr;
	wxSpinCtrl *visible_lines = nullptr;
	wxSpinCtrl *transition_ms = nullptr;
	wxSpinCtrl *margin_lr = nullptr;
	wxSpinCtrl *active_alpha = nullptr;
	wxSpinCtrl *inactive_alpha = nullptr;
	wxSpinCtrl *layer = nullptr;
	wxSpinCtrl *wrap_after = nullptr;
	wxSpinCtrl *outline_size = nullptr;
	wxSpinCtrl *shadow_size = nullptr;
	wxTextCtrl *active_color = nullptr;
	wxTextCtrl *inactive_color = nullptr;
	wxTextCtrl *outline_color = nullptr;
	SubtitlesPreview *active_preview = nullptr;
	SubtitlesPreview *inactive_preview = nullptr;
	LyricScrollSettings settings;
	int script_width = 3840;
	int script_height = 2160;

	wxSpinCtrl *spin(wxWindow *parent, int value, int min_value, int max_value) {
		return new wxSpinCtrl(parent, -1, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min_value, max_value, value);
	}

	void add_row(wxWindow *parent, wxFlexGridSizer *grid, wxString const& label, wxWindow *ctrl) {
		grid->Add(new wxStaticText(parent, -1, label), wxSizerFlags().Center().Right());
		grid->Add(ctrl, wxSizerFlags(1).Expand());
	}

	void UpdateResolutionPreset() {
		if (!resolution_preset)
			return;

		switch (resolution_preset->GetSelection()) {
			case 0:
				target_width->SetValue(script_width);
				target_height->SetValue(script_height);
				break;
			case 1:
				target_width->SetValue(3840);
				target_height->SetValue(2160);
				break;
			case 2:
				target_width->SetValue(1920);
				target_height->SetValue(1080);
				break;
			case 3:
				target_width->SetValue(1280);
				target_height->SetValue(720);
				break;
			case 4:
				target_width->SetValue(1080);
				target_height->SetValue(1920);
				break;
			case 5:
				target_width->SetValue(720);
				target_height->SetValue(1280);
				break;
			case 6:
				break;
			default:
				target_width->SetValue(script_width);
				target_height->SetValue(script_height);
				break;
		}
	}

	void CaptureSettings() {
		settings.scope = scope->GetSelection();
		settings.source_action = source_action->GetSelection();
		settings.clear_previous = clear_previous->GetValue();
		settings.strip_tags = strip_tags->GetValue();
		settings.preserve_line_breaks = preserve_line_breaks->GetValue();
		settings.animate = animate->GetValue();
		settings.resolution_preset = resolution_preset->GetSelection();
		settings.target_width = target_width->GetValue();
		settings.target_height = target_height->GetValue();
		settings.horizontal_alignment = horizontal_alignment->GetSelection();
		settings.center_x = center_x->GetValue();
		settings.center_y = center_y->GetValue();
		settings.line_gap = line_gap->GetValue();
		settings.active_size = active_size->GetValue();
		settings.inactive_size = inactive_size->GetValue();
		settings.visible_lines = visible_lines->GetValue();
		settings.transition_ms = transition_ms->GetValue();
		settings.margin_lr = margin_lr->GetValue();
		settings.active_alpha = active_alpha->GetValue();
		settings.inactive_alpha = inactive_alpha->GetValue();
		settings.layer = layer->GetValue();
		settings.wrap_after = wrap_after->GetValue();
		settings.outline_size = outline_size->GetValue();
		settings.shadow_size = shadow_size->GetValue();
		settings.active_color = normalize_ass_color(from_wx(active_color->GetValue()), "&HFFFFFF&");
		settings.inactive_color = normalize_ass_color(from_wx(inactive_color->GetValue()), "&HD8D8D8&");
		settings.outline_color = normalize_ass_color(from_wx(outline_color->GetValue()), "&H000000&");
	}

	void OnOK(wxCommandEvent&) {
		CaptureSettings();
		save_lyric_scroll_settings(settings);
		EndModal(wxID_OK);
	}

	std::string PreviewFont(char const *fallback) {
		auto selected = context->selectionController->GetSortedSelection();
		for (auto line : selected) {
			if (auto style = context->ass->GetStyle(line->Style))
				return style->font;
		}

		for (auto const& line : context->ass->Events) {
			if (!line.Comment) {
				if (auto style = context->ass->GetStyle(line.Style))
					return style->font;
			}
		}

		return fallback;
	}

	std::string PreviewText(bool current) {
		std::vector<std::string> samples;
		auto add_sample = [&](AssDialogue *line) {
			std::string text = escape_ass_text(plain_lyric_text(line, settings));
			if (!text.empty())
				samples.push_back(std::move(text));
		};

		for (auto line : context->selectionController->GetSortedSelection())
			add_sample(line);

		for (auto& line : context->ass->Events) {
			if (samples.size() >= 2)
				break;
			if (!line.Comment && line.Effect.get() != LYRIC_SCROLL_GENERATED)
				add_sample(&line);
		}

		size_t index = current ? 0 : 1;
		if (samples.size() > index)
			return samples[index];
		if (!samples.empty())
			return samples.front();
		return current ? "Current lyric line\\NTranslated line" : "Nearby lyric line\\NTranslated line";
	}

	AssStyle PreviewAssStyle(bool current) {
		auto resolution = resolve_scroll_resolution(context->ass.get(), settings);
		auto layout_settings = resolve_layout_settings(settings, resolution);
		double preview_scale = 560.0 / std::max(1, resolution.target_width);
		int source_size = current ? layout_settings.active_size : layout_settings.inactive_size;

		AssStyle style;
		style.font = PreviewFont("Arial");
		style.fontsize = std::max(10, std::min(44, static_cast<int>(std::lround(source_size * preview_scale * 1.2))));
		style.primary = agi::Color(current ? layout_settings.active_color : layout_settings.inactive_color);
		style.secondary = agi::Color("&H000000FF");
		style.outline = agi::Color(layout_settings.outline_color);
		style.shadow = agi::Color("&H00000000");
		style.bold = current;
		style.borderstyle = 1;
		style.outline_w = std::max(0, std::min(6, static_cast<int>(std::lround(layout_settings.outline_size * preview_scale * 1.2))));
		style.shadow_w = std::max(0, std::min(6, static_cast<int>(std::lround(layout_settings.shadow_size * preview_scale * 1.2))));
		style.alignment = lyric_ass_alignment(settings);
		style.encoding = 1;
		style.UpdateData();
		return style;
	}

	void UpdateStylePreview() {
		if (!active_preview || !inactive_preview)
			return;

		active_preview->SetStyle(PreviewAssStyle(true), false);
		active_preview->SetText(PreviewText(true));
		inactive_preview->SetStyle(PreviewAssStyle(false), false);
		inactive_preview->SetText(PreviewText(false));
	}

	void OnPreview(wxCommandEvent&) {
		CaptureSettings();
		save_lyric_scroll_settings(settings);
		UpdateStylePreview();
	}

public:
	DialogLyricScroll(wxWindow *parent, agi::Context *c)
	: wxDialog(parent, -1, _("Music Lyrics Scroll Generator"))
	, context(c)
	, settings(load_lyric_scroll_settings())
	{
		c->ass->GetResolution(script_width, script_height);
		if (script_width <= 0) script_width = 3840;
		if (script_height <= 0) script_height = 2160;

		auto notebook = new wxNotebook(this, -1);
		auto common_page = new wxPanel(notebook);
		auto advanced_page = new wxPanel(notebook);

		wxString scope_choices[] = { _("Selected lines only"), _("All dialogue lines") };
		scope = new wxRadioBox(common_page, -1, _("Scope"), wxDefaultPosition, wxDefaultSize, 2, scope_choices, 1, wxRA_SPECIFY_COLS);
		scope->SetSelection(settings.scope);

		source_action = new wxChoice(common_page, -1);
		source_action->Append(_("Keep original subtitles"));
		source_action->Append(_("Comment-hide originals (recommended)"));
		source_action->Append(_("Delete original subtitles"));
		source_action->SetSelection(settings.source_action);

		resolution_preset = new wxChoice(common_page, -1);
		resolution_preset->Append(_("Script PlayRes"));
		resolution_preset->Append(_("2160p / 4K"));
		resolution_preset->Append(_("1080p"));
		resolution_preset->Append(_("720p"));
		resolution_preset->Append(_("1080x1920 vertical"));
		resolution_preset->Append(_("720x1280 vertical"));
		resolution_preset->Append(_("Custom"));
		resolution_preset->SetSelection(settings.resolution_preset);

		horizontal_alignment = new wxChoice(common_page, -1);
		horizontal_alignment->Append(_("Left"));
		horizontal_alignment->Append(_("Center"));
		horizontal_alignment->Append(_("Right"));
		horizontal_alignment->SetSelection(settings.horizontal_alignment);

		clear_previous = new wxCheckBox(advanced_page, -1, _("Clear previous scroll results before regenerating"));
		clear_previous->SetValue(settings.clear_previous);
		strip_tags = new wxCheckBox(advanced_page, -1, _("Strip override tags from source subtitles"));
		strip_tags->SetValue(settings.strip_tags);
		preserve_line_breaks = new wxCheckBox(advanced_page, -1, _("Preserve manual \\N line breaks"));
		preserve_line_breaks->SetValue(settings.preserve_line_breaks);
		animate = new wxCheckBox(advanced_page, -1, _("Enable smooth scrolling animation"));
		animate->SetValue(settings.animate);

		target_width = spin(common_page, settings.target_width, 1, 10000);
		target_height = spin(common_page, settings.target_height, 1, 10000);
		center_x = spin(advanced_page, settings.center_x, 0, 10000);
		center_y = spin(common_page, settings.center_y, 0, 10000);
		line_gap = spin(common_page, settings.line_gap, 0, 1000);
		active_size = spin(common_page, settings.active_size, 0, 400);
		inactive_size = spin(common_page, settings.inactive_size, 0, 400);
		visible_lines = spin(common_page, settings.visible_lines, 0, 8);
		transition_ms = spin(common_page, settings.transition_ms, 0, 5000);
		margin_lr = spin(advanced_page, settings.margin_lr, 0, 3000);
		active_alpha = spin(advanced_page, settings.active_alpha, 0, 255);
		inactive_alpha = spin(advanced_page, settings.inactive_alpha, 0, 255);
		layer = spin(advanced_page, settings.layer, 0, 999);
		wrap_after = spin(common_page, settings.wrap_after, 0, 200);
		outline_size = spin(advanced_page, settings.outline_size, 0, 60);
		shadow_size = spin(advanced_page, settings.shadow_size, 0, 60);
		active_color = new wxTextCtrl(advanced_page, -1, to_wx(settings.active_color));
		inactive_color = new wxTextCtrl(advanced_page, -1, to_wx(settings.inactive_color));
		outline_color = new wxTextCtrl(advanced_page, -1, to_wx(settings.outline_color));

		auto common_grid = new wxFlexGridSizer(2, 8, 8);
		common_grid->AddGrowableCol(1, 1);
		add_row(common_page, common_grid, _("Source subtitle handling"), source_action);
		add_row(common_page, common_grid, _("Output resolution"), resolution_preset);
		add_row(common_page, common_grid, _("Output width"), target_width);
		add_row(common_page, common_grid, _("Output height"), target_height);
		add_row(common_page, common_grid, _("Horizontal alignment"), horizontal_alignment);
		add_row(common_page, common_grid, _("Current lyric Y position (0 auto)"), center_y);
		add_row(common_page, common_grid, _("Active line font size (0 auto)"), active_size);
		add_row(common_page, common_grid, _("Inactive line font size (0 auto)"), inactive_size);
		add_row(common_page, common_grid, _("Line gap (0 auto)"), line_gap);
		add_row(common_page, common_grid, _("Visible surrounding lines"), visible_lines);
		add_row(common_page, common_grid, _("Scroll transition (ms)"), transition_ms);
		add_row(common_page, common_grid, _("Wrap lyrics after N chars"), wrap_after);

		auto preview_box = new wxStaticBoxSizer(wxVERTICAL, common_page, _("Style preview"));
		active_preview = new SubtitlesPreview(common_page, wxSize(520, 86), wxSUNKEN_BORDER, agi::Color(48, 48, 48));
		inactive_preview = new SubtitlesPreview(common_page, wxSize(520, 70), wxSUNKEN_BORDER, agi::Color(48, 48, 48));
		preview_box->Add(active_preview, wxSizerFlags().Expand().Border(wxALL, 4));
		preview_box->Add(inactive_preview, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM, 4));

		auto common_sizer = new wxBoxSizer(wxVERTICAL);
		common_sizer->Add(scope, wxSizerFlags().Expand().Border());
		common_sizer->Add(common_grid, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		common_sizer->Add(preview_box, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		common_page->SetSizer(common_sizer);

		auto advanced_grid = new wxFlexGridSizer(2, 8, 8);
		advanced_grid->AddGrowableCol(1, 1);
		add_row(advanced_page, advanced_grid, _("X anchor position (0 auto)"), center_x);
		add_row(advanced_page, advanced_grid, _("Left/right margin (0 auto)"), margin_lr);
		add_row(advanced_page, advanced_grid, _("Active line alpha"), active_alpha);
		add_row(advanced_page, advanced_grid, _("Inactive line alpha"), inactive_alpha);
		add_row(advanced_page, advanced_grid, _("Layer"), layer);
		add_row(advanced_page, advanced_grid, _("Active line color"), active_color);
		add_row(advanced_page, advanced_grid, _("Inactive line color"), inactive_color);
		add_row(advanced_page, advanced_grid, _("Border size"), outline_size);
		add_row(advanced_page, advanced_grid, _("Shadow size"), shadow_size);
		add_row(advanced_page, advanced_grid, _("Border color"), outline_color);

		auto advanced_sizer = new wxBoxSizer(wxVERTICAL);
		advanced_sizer->Add(advanced_grid, wxSizerFlags(1).Expand().Border());
		advanced_sizer->Add(clear_previous, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		advanced_sizer->Add(strip_tags, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		advanced_sizer->Add(preserve_line_breaks, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		advanced_sizer->Add(animate, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		advanced_page->SetSizer(advanced_sizer);

		notebook->AddPage(common_page, _("Common"), true);
		notebook->AddPage(advanced_page, _("Advanced"));

		UpdateResolutionPreset();

		auto main = new wxBoxSizer(wxVERTICAL);
		main->Add(notebook, wxSizerFlags(1).Expand().Border());
		auto button_sizer = new wxBoxSizer(wxHORIZONTAL);
		button_sizer->Add(new wxButton(this, wxID_APPLY, _("Update Preview")), wxSizerFlags().Border(wxRIGHT));
		button_sizer->AddStretchSpacer();
		button_sizer->Add(new wxButton(this, wxID_OK), wxSizerFlags().Border(wxRIGHT));
		button_sizer->Add(new wxButton(this, wxID_CANCEL));
		main->Add(button_sizer, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		SetSizerAndFit(main);
		SetMinSize(wxSize(540, -1));
		CenterOnParent();
		Bind(wxEVT_BUTTON, &DialogLyricScroll::OnOK, this, wxID_OK);
		Bind(wxEVT_BUTTON, &DialogLyricScroll::OnPreview, this, wxID_APPLY);
		resolution_preset->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { UpdateResolutionPreset(); });
		UpdateStylePreview();
	}

	LyricScrollSettings const& GetSettings() const { return settings; }
};

std::vector<AssDialogue *> collect_lyric_sources(agi::Context *c, LyricScrollSettings const& settings) {
	std::vector<AssDialogue *> lines;
	if (settings.scope == 0) {
		lines = c->selectionController->GetSortedSelection();
	}
	else {
		for (auto& line : c->ass->Events) {
			if (!line.Comment || line.Effect.get() == LYRIC_SCROLL_SOURCE)
				lines.push_back(&line);
		}
	}

	lines.erase(std::remove_if(lines.begin(), lines.end(), [](AssDialogue *line) {
		return line->Effect.get() == LYRIC_SCROLL_GENERATED || trim_copy(line->GetStrippedText()).empty();
	}), lines.end());
	return lines;
}

std::string replace_all(std::string text, std::string const& from, std::string const& to) {
	size_t pos = 0;
	while ((pos = text.find(from, pos)) != std::string::npos) {
		text.replace(pos, from.size(), to);
		pos += to.size();
	}
	return text;
}

std::string collapse_spaces(std::string text) {
	std::string out;
	bool had_space = false;
	for (unsigned char c : text) {
		if (std::isspace(c)) {
			if (!had_space && !out.empty())
				out += ' ';
			had_space = true;
		}
		else {
			out += c;
			had_space = false;
		}
	}
	return trim_copy(out);
}

std::string normalize_line_breaks(std::string text) {
	text = replace_all(std::move(text), "\r\n", "\\N");
	text = replace_all(std::move(text), "\r", "\\N");
	text = replace_all(std::move(text), "\n", "\\N");
	text = replace_all(std::move(text), "\\n", "\\N");
	return text;
}

bool ends_with_ass_line_break(std::string const& text) {
	return text.size() >= 2 && text.compare(text.size() - 2, 2, "\\N") == 0;
}

std::string collapse_spaces_preserving_line_breaks(std::string text) {
	std::string out;
	size_t segment_start = 0;
	while (true) {
		size_t break_pos = text.find("\\N", segment_start);
		std::string segment = collapse_spaces(text.substr(segment_start, break_pos == std::string::npos ? std::string::npos : break_pos - segment_start));
		if (!segment.empty()) {
			if (!out.empty() && !ends_with_ass_line_break(out))
				out += ' ';
			out += segment;
		}

		if (break_pos == std::string::npos)
			break;

		if (!out.empty() && !ends_with_ass_line_break(out))
			out += "\\N";
		segment_start = break_pos + 2;
	}
	while (ends_with_ass_line_break(out))
		out.erase(out.size() - 2);
	return out;
}

std::string plain_lyric_text(AssDialogue *line, LyricScrollSettings const& settings) {
	std::string text = settings.strip_tags ? line->GetStrippedText() : clean_motion_conflicts(line->Text.get());
	if (settings.preserve_line_breaks)
		return wrap_text(collapse_spaces_preserving_line_breaks(normalize_line_breaks(std::move(text))), settings.wrap_after);

	text = replace_all(std::move(text), "\r\n", " ");
	text = replace_all(std::move(text), "\r", " ");
	text = replace_all(std::move(text), "\n", " ");
	text = replace_all(std::move(text), "\\N", " ");
	text = replace_all(std::move(text), "\\n", " ");
	return wrap_text(collapse_spaces(std::move(text)), settings.wrap_after);
}

std::string escape_ass_text(std::string text) {
	text.erase(std::remove(text.begin(), text.end(), '{'), text.end());
	text.erase(std::remove(text.begin(), text.end(), '}'), text.end());
	text = replace_all(std::move(text), "\r\n", "\\N");
	text = replace_all(std::move(text), "\n", "\\N");
	return text;
}

struct LyricRow {
	int start = 0;
	int end = 0;
	std::string primary;
	std::string secondary;
	AssDialogue *line = nullptr;
};

struct CreditRow {
	int start = 0;
	int end = 0;
	std::string text;
	AssDialogue *line = nullptr;
};

std::string most_common_style(std::map<std::string, int> const& counts) {
	return std::max_element(counts.begin(), counts.end(), [](auto const& a, auto const& b) {
		return a.second < b.second;
	})->first;
}

void normalize_row_end_times(std::vector<LyricRow>& rows, int default_duration_ms) {
	for (size_t i = 0; i < rows.size(); ++i) {
		int next_start = i + 1 < rows.size() ? rows[i + 1].start : rows[i].start + default_duration_ms;
		if (rows[i].end <= rows[i].start)
			rows[i].end = next_start;
		rows[i].end = std::min(std::max(rows[i].end, rows[i].start + 250), next_start);
	}
}

void build_lyric_rows(std::vector<AssDialogue *> const& sources, LyricScrollSettings const& settings, std::vector<LyricRow>& rows, std::vector<CreditRow>& credits, std::string& primary_style, std::string& secondary_style, std::string& credit_style) {
	std::set<std::string> styles_seen;
	std::map<std::string, int> style_counts;
	std::map<std::pair<int, int>, std::map<std::string, std::pair<std::string, AssDialogue *>>> grouped;
	credit_style = "制作人员";

	for (auto line : sources) {
		std::string text = plain_lyric_text(line, settings);
		if (text.empty())
			continue;

		std::string style = line->Style.get();
		styles_seen.insert(style);
		if (style == credit_style) {
			credits.push_back({static_cast<int>(line->Start), static_cast<int>(line->End), text, line});
			continue;
		}

		++style_counts[style];
		grouped[{static_cast<int>(line->Start), static_cast<int>(line->End)}][style] = {text, line};
	}

	if (styles_seen.count("中"))
		primary_style = "中";
	else if (!style_counts.empty())
		primary_style = most_common_style(style_counts);

	if (styles_seen.count("英") && primary_style != "英")
		secondary_style = "英";

	for (auto const& group : grouped) {
		auto const& by_style = group.second;
		auto primary_it = by_style.find(primary_style);
		if (primary_it == by_style.end()) {
			if (primary_style.empty() && !by_style.empty())
				primary_it = by_style.begin();
			else
				continue;
		}

		std::string secondary;
		if (!secondary_style.empty()) {
			auto secondary_it = by_style.find(secondary_style);
			if (secondary_it != by_style.end())
				secondary = secondary_it->second.first;
		}

		rows.push_back({
			group.first.first,
			group.first.second,
			primary_it->second.first,
			secondary,
			primary_it->second.second
		});
	}

	normalize_row_end_times(rows, 3500);
}

double lerp(double left, double right, double progress) {
	return left + (right - left) * progress;
}

double ease_out_cubic(double value) {
	value = std::max(0.0, std::min(1.0, value));
	return 1.0 - std::pow(1.0 - value, 3.0);
}

double interpolate_points(std::vector<std::pair<double, double>> const& points, double value) {
	value = std::abs(value);
	if (value <= points.front().first)
		return points.front().second;

	for (size_t i = 1; i < points.size(); ++i) {
		double x0 = points[i - 1].first;
		double y0 = points[i - 1].second;
		double x1 = points[i].first;
		double y1 = points[i].second;
		if (value <= x1)
			return lerp(y0, y1, (value - x0) / (x1 - x0));
	}
	return points.back().second;
}

int alpha_for_offset(double offset, LyricScrollSettings const& settings) {
	return static_cast<int>(std::lround(interpolate_points({{0, static_cast<double>(settings.active_alpha)}, {1, static_cast<double>(settings.inactive_alpha)}, {2, 0x92}, {3, 0xC0}, {4, 0xE0}}, offset)));
}

double scale_for_offset(double offset) {
	return interpolate_points({{0, 100}, {1, 78}, {2, 64}, {3, 54}, {4, 50}}, offset);
}

std::string color_for_offset(double offset) {
	int gray = static_cast<int>(std::lround(interpolate_points({{0, 255}, {1, 216}, {4, 190}}, offset)));
	char buffer[16];
	std::snprintf(buffer, sizeof buffer, "&H%02X%02X%02X&", gray, gray, gray);
	return buffer;
}

std::vector<std::tuple<int, int, double, double>> split_transition(int start, int end, int motion_steps) {
	int duration = end - start;
	if (duration <= 0)
		return {};

	int steps = std::max(1, std::min(motion_steps, std::max(1, duration / 15)));
	std::vector<std::tuple<int, int, double, double>> spans;
	int previous = start;
	for (int i = 0; i < steps; ++i) {
		int current = start + static_cast<int>(std::lround(duration * (i + 1.0) / steps));
		if (current <= previous)
			continue;

		double start_progress = ease_out_cubic(static_cast<double>(i) / steps);
		double end_progress = ease_out_cubic((i + 1.0) / steps);
		spans.emplace_back(previous, current, start_progress, end_progress);
		previous = current;
	}
	return spans;
}

void upsert_scroll_style(AssFile *ass, std::string const& name, std::string const& font, int size, std::string const& primary, int margin, int outline, int shadow, std::string const& outline_color, int alignment) {
	AssStyle *style = ass->GetStyle(name);
	if (!style) {
		style = new AssStyle;
		style->name = name;
		ass->Styles.push_back(*style);
	}

	style->font = font.empty() ? "Arial" : font;
	style->fontsize = size;
	style->primary = agi::Color(primary);
	style->secondary = agi::Color("&H000000FF");
	style->outline = agi::Color(outline_color);
	style->shadow = agi::Color("&H00000000");
	style->bold = false;
	style->italic = false;
	style->underline = false;
	style->strikeout = false;
	style->scalex = 100;
	style->scaley = 100;
	style->spacing = 0;
	style->angle = 0;
	style->borderstyle = 1;
	style->outline_w = outline;
	style->shadow_w = shadow;
	style->alignment = alignment;
	style->Margin[0] = margin;
	style->Margin[1] = margin;
	style->Margin[2] = 0;
	style->encoding = 1;
	style->UpdateData();
}

std::string style_font(AssFile *ass, std::string const& style_name, char const *fallback) {
	if (auto style = ass->GetStyle(style_name))
		return style->font;
	return fallback;
}

double lyric_wrap_unit(std::string const& ch) {
	if (ch.empty())
		return 0.0;
	unsigned char first = static_cast<unsigned char>(ch[0]);
	if (ch.size() == 1 && std::isspace(first))
		return 0.35;
	if (ch.size() == 1 && std::ispunct(first))
		return 0.45;
	if (first < 0x80)
		return 0.58;
	return 1.0;
}

std::string wrap_by_units(std::string text, int max_units) {
	if (max_units <= 0)
		return text;

	size_t line_start = 0;
	size_t last_space = std::string::npos;
	double units = 0.0;
	for (size_t pos = 0; pos < text.size();) {
		if (text.compare(pos, 2, "\\N") == 0 || text[pos] == '\n') {
			pos += text[pos] == '\n' ? 1 : 2;
			line_start = pos;
			last_space = std::string::npos;
			units = 0.0;
			continue;
		}

		size_t len = utf8_char_len(static_cast<unsigned char>(text[pos]));
		if (pos + len > text.size())
			len = 1;
		std::string ch = text.substr(pos, len);
		if (len == 1 && std::isspace(static_cast<unsigned char>(text[pos])))
			last_space = pos;

		double next_units = units + lyric_wrap_unit(ch);
		if (next_units > max_units && pos > line_start) {
			size_t break_pos = last_space != std::string::npos && last_space > line_start ? last_space : pos;
			text.replace(break_pos, last_space == break_pos ? 1 : 0, "\\N");
			pos = break_pos + 2;
			line_start = pos;
			last_space = std::string::npos;
			units = 0.0;
			continue;
		}

		units = next_units;
		pos += len;
	}
	return text;
}

int count_ass_lines(std::string const& text) {
	if (text.empty())
		return 0;

	int lines = 1;
	for (size_t pos = 0; (pos = text.find("\\N", pos)) != std::string::npos; pos += 2)
		++lines;
	return lines;
}

struct LyricRenderLayout {
	int primary_wrap_units = 24;
	int secondary_wrap_units = 34;
	double padding = 40.0;
};

LyricRenderLayout make_render_layout(LyricScrollSettings const& settings, int width, int height) {
	bool portrait = height > width;
	double center_distance = std::abs(settings.center_x - width / 2.0);
	double region_width = portrait ? width * 0.84 : (center_distance > width * 0.12 ? width * 0.46 : width * 0.72);
	LyricRenderLayout layout;
	layout.primary_wrap_units = std::max(8, static_cast<int>(std::floor(region_width / std::max(1.0, settings.active_size * 0.45))));
	int secondary_size = std::max(6, static_cast<int>(std::lround(settings.active_size * 72.0 / 165.0)));
	layout.secondary_wrap_units = std::max(10, static_cast<int>(std::floor(region_width / std::max(1.0, secondary_size * 0.44))));
	layout.padding = std::max(12.0, settings.line_gap * 0.18);
	return layout;
}

std::string wrapped_primary(LyricRow const& row, LyricRenderLayout const& layout) {
	return wrap_by_units(row.primary, layout.primary_wrap_units);
}

std::string wrapped_secondary(LyricRow const& row, LyricRenderLayout const& layout) {
	return wrap_by_units(row.secondary, layout.secondary_wrap_units);
}

double row_visual_height(LyricRow const& row, bool current, LyricScrollSettings const& settings, LyricRenderLayout const& layout) {
	int primary_size = current ? settings.active_size : settings.inactive_size;
	int secondary_size = current ?
		std::max(6, static_cast<int>(std::lround(settings.active_size * 72.0 / 165.0))) :
		std::max(6, static_cast<int>(std::lround(settings.inactive_size * 58.0 / 128.0)));

	int primary_lines = std::max(1, count_ass_lines(wrapped_primary(row, layout)));
	int secondary_lines = row.secondary.empty() ? 0 : std::max(1, count_ass_lines(wrapped_secondary(row, layout)));
	double height = primary_lines * primary_size * 1.08;
	if (secondary_lines)
		height += primary_size * 0.18 + secondary_lines * secondary_size * 1.05;
	return height + settings.outline_size * 2.0;
}

double center_distance_between(double first_height, double second_height, LyricScrollSettings const& settings, LyricRenderLayout const& layout) {
	return std::max<double>(settings.line_gap, first_height / 2.0 + second_height / 2.0 + layout.padding);
}

double row_center_y(std::vector<LyricRow> const& rows, int active_index, int row_index, int center, LyricScrollSettings const& settings, LyricRenderLayout const& layout) {
	if (row_index == active_index)
		return center;

	double y = center;
	double previous_height = row_visual_height(rows[active_index], true, settings, layout);
	if (row_index > active_index) {
		for (int i = active_index + 1; i <= row_index; ++i) {
			double height = row_visual_height(rows[i], false, settings, layout);
			y += center_distance_between(previous_height, height, settings, layout);
			previous_height = height;
		}
		return y;
	}

	for (int i = active_index - 1; i >= row_index; --i) {
		double height = row_visual_height(rows[i], false, settings, layout);
		y -= center_distance_between(previous_height, height, settings, layout);
		previous_height = height;
	}
	return y;
}

std::string build_body(LyricRow const& row, bool current, LyricScrollSettings const& settings, LyricRenderLayout const& layout) {
	std::string primary = escape_ass_text(wrapped_primary(row, layout));
	std::string secondary = escape_ass_text(wrapped_secondary(row, layout));
	std::string alignment = "\\an" + std::to_string(lyric_ass_alignment(settings));
	if (current) {
		std::string body = "{" + alignment + "\\blur0.5\\fsp0}" + primary;
		if (!secondary.empty()) {
			int secondary_size = std::max(6, static_cast<int>(std::lround(settings.active_size * 72.0 / 165.0)));
			body += "\\N{\\fs" + std::to_string(secondary_size) + "\\alpha&H42&}" + secondary;
		}
		return body;
	}

	std::string body = "{" + alignment + "\\blur1\\fsp0}" + primary;
	if (!secondary.empty()) {
		int secondary_size = std::max(6, static_cast<int>(std::lround(settings.inactive_size * 58.0 / 128.0)));
		body += "\\N{\\fs" + std::to_string(secondary_size) + "\\alpha&H78&}" + secondary;
	}
	return body;
}

AssDialogue *add_scroll_dialogue(agi::Context *c, AssDialogue *base, int layer, int start, int end, std::string const& style, std::string const& text) {
	if (end <= start)
		return nullptr;

	auto generated = new AssDialogue(*base);
	generated->Comment = false;
	generated->Layer = layer;
	generated->Start = start;
	generated->End = end;
	generated->Style = style;
	generated->Effect = LYRIC_SCROLL_GENERATED;
	generated->Margin[0] = 0;
	generated->Margin[1] = 0;
	generated->Margin[2] = 0;
	generated->Text = text;
	c->ass->Events.push_back(*generated);
	return generated;
}

AssDialogue *add_position_dialogue(agi::Context *c, LyricRow const& row, int layer, int start, int end, double y, double scale, int alpha, std::string const& color, bool current, int x, LyricScrollSettings const& settings, LyricRenderLayout const& layout) {
	char buffer[256];
	std::snprintf(buffer, sizeof buffer, "{\\an%d\\q2\\pos(%d,%d)\\alpha&H%02X&\\1c%s\\fscx%.1f\\fscy%.1f}",
		lyric_ass_alignment(settings), x, static_cast<int>(std::lround(y)), std::max(0, std::min(255, alpha)), color.c_str(), scale, scale);
	return add_scroll_dialogue(c, row.line, layer, start, end, current ? "ScrollCurrent" : "ScrollDim", std::string(buffer) + build_body(row, current, settings, layout));
}

AssDialogue *add_moving_dialogue(agi::Context *c, LyricRow const& row, int layer, int start, int end, double y0, double y1, double scale0, double scale1, int alpha0, int alpha1, std::string const& color0, std::string const& color1, bool current, int x, LyricScrollSettings const& settings, LyricRenderLayout const& layout) {
	int duration = std::max(1, end - start);
	char buffer[512];
	std::snprintf(buffer, sizeof buffer,
		"{\\an%d\\q2\\move(%d,%d,%d,%d)\\alpha&H%02X&\\1c%s\\fscx%.1f\\fscy%.1f\\t(0,%d,1.0,\\alpha&H%02X&\\1c%s\\fscx%.1f\\fscy%.1f)}",
		lyric_ass_alignment(settings), x, static_cast<int>(std::lround(y0)), x, static_cast<int>(std::lround(y1)),
		std::max(0, std::min(255, alpha0)), color0.c_str(), scale0, scale0,
		duration, std::max(0, std::min(255, alpha1)), color1.c_str(), scale1, scale1);
	return add_scroll_dialogue(c, row.line, layer, start, end, current ? "ScrollCurrent" : "ScrollDim", std::string(buffer) + build_body(row, current, settings, layout));
}

void generate_scroll_events(agi::Context *c, std::vector<LyricRow> const& rows, std::vector<CreditRow> const& credits, LyricScrollSettings const& settings, Selection& new_selection, AssDialogue *&new_active) {
	int width = 0;
	int height = 0;
	c->ass->GetResolution(width, height);
	if (width <= 0) width = 3840;
	if (height <= 0) height = 2160;

	int x = settings.center_x > 0 ? settings.center_x : width / 2;
	int center = settings.center_y > 0 ? settings.center_y : height / 2;
	int window = settings.visible_lines;
	int motion_steps = 28;
	auto layout = make_render_layout(settings, width, height);

	for (auto const& credit : credits) {
		std::string text = escape_ass_text(replace_all(credit.text, "\\N", " / "));
		auto generated = add_scroll_dialogue(c, credit.line, settings.layer + 2, credit.start, credit.end, "ScrollCredit",
			"{\\an" + std::to_string(lyric_ass_alignment(settings)) + "\\pos(" + std::to_string(x) + "," + std::to_string(static_cast<int>(std::lround(height * 0.176))) + ")\\alpha&H20&}" + text);
		if (generated) {
			new_selection.insert(generated);
			if (!new_active) new_active = generated;
		}
	}

	for (size_t active_index = 0; active_index < rows.size(); ++active_index) {
		auto const& row = rows[active_index];
		int start = row.start;
		int end = active_index + 1 < rows.size() ? rows[active_index + 1].start : row.end;
		end = std::max(end, start + 250);
		int transition_end = settings.animate && active_index > 0 ? std::min(end, start + settings.transition_ms) : start;

		int transition_start_row = std::max(0, static_cast<int>(active_index) - window - 1);
		int transition_end_row = std::min(static_cast<int>(rows.size()), static_cast<int>(active_index) + window + 1);
		for (auto const& span : split_transition(start, transition_end, motion_steps)) {
			int span_start;
			int span_end;
			double start_progress;
			double end_progress;
			std::tie(span_start, span_end, start_progress, end_progress) = span;
			for (int row_index = transition_start_row; row_index < transition_end_row; ++row_index) {
				int target_offset = row_index - static_cast<int>(active_index);
				int previous_offset = row_index - (static_cast<int>(active_index) - 1);
				int previous_active = static_cast<int>(active_index) - 1;
				bool was_visible = previous_active >= 0 && std::abs(previous_offset) <= window;
				bool is_visible = std::abs(target_offset) <= window;
				if (!was_visible && !is_visible)
					continue;

				double visible_target_y = row_center_y(rows, static_cast<int>(active_index), row_index, center, settings, layout);
				double start_y = was_visible ?
					row_center_y(rows, previous_active, row_index, center, settings, layout) :
					visible_target_y + (target_offset >= 0 ? settings.line_gap : -settings.line_gap);
				double target_y = is_visible ?
					visible_target_y :
					start_y + (target_offset < previous_offset ? -settings.line_gap : settings.line_gap);
				int start_offset = was_visible ? previous_offset : target_offset + (target_offset >= 0 ? 1 : -1);
				int end_offset = is_visible ? target_offset : previous_offset + (target_offset < previous_offset ? -1 : 1);
				double start_float_offset = lerp(start_offset, target_offset, start_progress);
				double end_float_offset = lerp(start_offset, end_offset, end_progress);
				bool current = is_visible && target_offset == 0;
				auto generated = add_moving_dialogue(c, rows[row_index], current ? settings.layer : std::max(0, settings.layer - 1),
					span_start, span_end, lerp(start_y, target_y, start_progress), lerp(start_y, target_y, end_progress),
					scale_for_offset(start_float_offset), scale_for_offset(end_float_offset),
					alpha_for_offset(start_float_offset, settings), alpha_for_offset(end_float_offset, settings),
					color_for_offset(start_float_offset), color_for_offset(end_float_offset), current, x, settings, layout);
				if (generated) {
					new_selection.insert(generated);
					if (!new_active) new_active = generated;
				}
			}
		}

		int hold_start = transition_end > start ? transition_end : start;
		if (hold_start >= end)
			continue;

		int visible_start = std::max(0, static_cast<int>(active_index) - window);
		int visible_end = std::min(static_cast<int>(rows.size()), static_cast<int>(active_index) + window + 1);
		for (int row_index = visible_start; row_index < visible_end; ++row_index) {
			int offset = row_index - static_cast<int>(active_index);
			bool current = offset == 0;
			auto generated = add_position_dialogue(c, rows[row_index], current ? settings.layer : std::max(0, settings.layer - 1),
				hold_start, end, row_center_y(rows, static_cast<int>(active_index), row_index, center, settings, layout), scale_for_offset(offset),
				alpha_for_offset(offset, settings), color_for_offset(offset), current, x, settings, layout);
			if (generated) {
				new_selection.insert(generated);
				if (!new_active) new_active = generated;
			}
		}
	}
}

void apply_lyric_scroll(agi::Context *c, LyricScrollSettings const& settings) {
	auto sources = collect_lyric_sources(c, settings);
	bool removed_previous = false;
	if (settings.clear_previous) {
		c->ass->Events.remove_and_dispose_if([](AssDialogue const& line) {
			return line.Effect.get() == LYRIC_SCROLL_GENERATED;
		}, [&](AssDialogue *line) {
			removed_previous = true;
			delete line;
		});
	}

	if (sources.empty()) {
		if (removed_previous)
			c->ass->Commit(_("clear music lyric scroll"), AssFile::COMMIT_DIAG_ADDREM);
		wxMessageBox(_("No subtitle lines were available for lyric scrolling."), _("Music Lyrics Scroll"), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	Selection new_selection;
	AssDialogue *new_active = nullptr;
	std::vector<LyricRow> rows;
	std::vector<CreditRow> credits;
	std::string primary_style;
	std::string secondary_style;
	std::string credit_style;
	build_lyric_rows(sources, settings, rows, credits, primary_style, secondary_style, credit_style);

	if (rows.empty() && credits.empty()) {
		wxMessageBox(_("No lyric rows were available after grouping source subtitles."), _("Music Lyrics Scroll"), wxOK | wxICON_INFORMATION, c->parent);
		return;
	}

	auto resolution = resolve_scroll_resolution(c->ass.get(), settings);
	if (resolution.target_width != resolution.script_width || resolution.target_height != resolution.script_height) {
		c->ass->SetScriptInfo("PlayResX", std::to_string(resolution.target_width));
		c->ass->SetScriptInfo("PlayResY", std::to_string(resolution.target_height));
	}
	auto layout_settings = resolve_layout_settings(settings, resolution);

	std::string primary_font = style_font(c->ass.get(), primary_style, "思源黑体 CN Heavy");
	std::string secondary_font = style_font(c->ass.get(), secondary_style, "思源黑体 CN Medium");
	int alignment = lyric_ass_alignment(layout_settings);
	upsert_scroll_style(c->ass.get(), "ScrollCurrent", primary_font, layout_settings.active_size, layout_settings.active_color, layout_settings.margin_lr, layout_settings.outline_size, layout_settings.shadow_size, layout_settings.outline_color, alignment);
	upsert_scroll_style(c->ass.get(), "ScrollDim", secondary_font.empty() ? primary_font : secondary_font, layout_settings.inactive_size, layout_settings.inactive_color, layout_settings.margin_lr, layout_settings.outline_size, layout_settings.shadow_size, layout_settings.outline_color, alignment);
	upsert_scroll_style(c->ass.get(), "ScrollCredit", primary_font, std::max(18, static_cast<int>(std::lround(layout_settings.active_size * 120.0 / 165.0))), layout_settings.active_color, layout_settings.margin_lr, layout_settings.outline_size, layout_settings.shadow_size, layout_settings.outline_color, alignment);
	generate_scroll_events(c, rows, credits, layout_settings, new_selection, new_active);

	if (settings.source_action == 1) {
		for (auto line : sources) {
			line->Comment = true;
			line->Effect = LYRIC_SCROLL_SOURCE;
		}
	}
	else if (settings.source_action == 2) {
		Selection source_set(sources.begin(), sources.end());
		c->ass->Events.remove_and_dispose_if([&](AssDialogue const& line) {
			return source_set.count(const_cast<AssDialogue *>(&line)) != 0;
		}, [](AssDialogue *line) {
			delete line;
		});
	}
	else {
		for (auto line : sources) {
			if (line->Effect.get() == LYRIC_SCROLL_SOURCE)
				line->Effect = "";
			line->Comment = false;
		}
	}

	c->ass->Commit(_("music lyric scroll"), AssFile::COMMIT_DIAG_ADDREM | AssFile::COMMIT_DIAG_FULL);
	if (new_active)
		c->selectionController->SetSelectionAndActive(std::move(new_selection), new_active);
}

struct tool_lyric_scroll final : public Command {
	CMD_NAME("tool/lyrics_scroll")
	CMD_ICON(timing_processor_toolbutton)
	STR_MENU("Music Lyrics Scroll(&L)...")
	STR_DISP("Music Lyrics Scroll")
	STR_HELP("Generate music-player-style scrolling lyric subtitles")

	void operator()(agi::Context *c) override {
		DialogLyricScroll dialog(c->parent, c);
		if (dialog.ShowModal() == wxID_OK)
			apply_lyric_scroll(c, dialog.GetSettings());
	}
};

struct tool_export final : public Command {
	CMD_NAME("tool/export")
	CMD_ICON(export_menu)
	STR_MENU("&Export Subtitles...")
	STR_DISP("Export Subtitles")
	STR_HELP("Save a copy of subtitles in a different format or with processing applied to it")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ShowExportDialog(c);
	}
};

struct tool_font_collector final : public Command {
	CMD_NAME("tool/font_collector")
	CMD_ICON(font_collector_button)
	STR_MENU("&Fonts Collector...")
	STR_DISP("Fonts Collector")
	STR_HELP("Open fonts collector")

	void operator()(agi::Context *c) override {
		ShowFontsCollectorDialog(c);
	}
};

struct tool_line_select final : public Command {
	CMD_NAME("tool/line/select")
	CMD_ICON(select_lines_button)
	STR_MENU("S&elect Lines...")
	STR_DISP("Select Lines")
	STR_HELP("Select lines based on defined criteria")

	void operator()(agi::Context *c) override {
		ShowSelectLinesDialog(c);
	}
};

struct tool_resampleres final : public Command {
	CMD_NAME("tool/resampleres")
	CMD_ICON(resample_toolbutton)
	STR_MENU("&Resample Resolution...")
	STR_DISP("Resample Resolution")
	STR_HELP("Resample subtitles to maintain their current appearance at a different script resolution")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		ResampleSettings settings;
		if (PromptForResampleSettings(c, settings))
			ResampleResolution(c->ass.get(), settings);
	}
};

struct tool_style_assistant final : public Command {
	CMD_NAME("tool/style/assistant")
	CMD_ICON(styling_toolbutton)
	STR_MENU("St&yling Assistant...")
	STR_DISP("Styling Assistant")
	STR_HELP("Open styling assistant")

	void operator()(agi::Context *c) override {
		c->dialog->Show<DialogStyling>(c);
	}
};

struct tool_styling_assistant_validator : public Command {
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return !!c->dialog->Get<DialogStyling>();
	}
};

struct tool_styling_assistant_commit final : public tool_styling_assistant_validator {
	CMD_NAME("tool/styling_assistant/commit")
	STR_MENU("&Accept changes")
	STR_DISP("Accept changes")
	STR_HELP("Commit changes and move to the next line")

	void operator()(agi::Context *c) override {
		c->dialog->Get<DialogStyling>()->Commit(true);
	}
};

struct tool_styling_assistant_preview final : public tool_styling_assistant_validator {
	CMD_NAME("tool/styling_assistant/preview")
	STR_MENU("&Preview changes")
	STR_DISP("Preview changes")
	STR_HELP("Commit changes and stay on the current line")

	void operator()(agi::Context *c) override {
		c->dialog->Get<DialogStyling>()->Commit(false);
	}
};

struct tool_style_manager final : public Command {
	CMD_NAME("tool/style/manager")
	CMD_ICON(style_toolbutton)
	STR_MENU("&Styles Manager...")
	STR_DISP("Styles Manager")
	STR_HELP("Open the styles manager")

	void operator()(agi::Context *c) override {
		ShowStyleManagerDialog(c);
	}
};

struct tool_time_kanji final : public Command {
	CMD_NAME("tool/time/kanji")
	CMD_ICON(kara_timing_copier)
	STR_MENU("&Kanji Timer...")
	STR_DISP("Kanji Timer")
	STR_HELP("Open the Kanji timer copier")

	void operator()(agi::Context *c) override {
		ShowKanjiTimerDialog(c);
	}
};

struct tool_time_stitch final : public Command {
	CMD_NAME("tool/time/stitch")
	CMD_ICON(shift_times_toolbutton)
	STR_MENU("Stitch &Timings...")
	STR_DISP("Stitch Timings")
	STR_HELP("Stitch adjacent subtitle timing gaps")

	void operator()(agi::Context *c) override {
		ShowStitchTimingsDialog(c);
	}
};

struct tool_style_overlap_check final : public Command {
	CMD_NAME("tool/style/overlap_check")
	STR_MENU("Check &Style Overlaps...")
	STR_DISP("Check Style Overlaps")
	STR_HELP("Check overlapping subtitle lines within each style")

	void operator()(agi::Context *c) override {
		ShowStyleOverlapCheckDialog(c);
	}
};

struct tool_text_cleanup final : public Command {
	CMD_NAME("tool/text/cleanup")
	STR_MENU("Subtitle &Text Cleanup...")
	STR_DISP("Subtitle Text Cleanup")
	STR_HELP("Clean Chinese punctuation and consecutive spaces in subtitle text")

	void operator()(agi::Context *c) override {
		ShowSubtitleTextCleanupDialog(c);
	}
};

struct tool_text_chinese_convert final : public Command {
	CMD_NAME("tool/text/chinese_convert")
	STR_MENU("Chinese &Simplified/Traditional Conversion...")
	STR_DISP("Chinese Simplified/Traditional Conversion")
	STR_HELP("Convert subtitle text between Simplified and Traditional Chinese")

	void operator()(agi::Context *c) override {
		ShowChineseConversionDialog(c);
	}
};

struct tool_text_pair_check final : public Command {
	CMD_NAME("tool/text/pair_check")
	STR_MENU("Check Paired &Punctuation...")
	STR_DISP("Check Paired Punctuation")
	STR_HELP("Check quotes, brackets and book-title marks for pairing problems")

	void operator()(agi::Context *c) override {
		ShowPairCheckDialog(c);
	}
};

struct tool_ai_analysis_settings final : public Command {
	CMD_NAME("tool/ai/analysis_settings")
	STR_MENU("AI Grammar Analysis &Settings...")
	STR_DISP("AI Grammar Analysis Settings")
	STR_HELP("Configure OpenAI-compatible AI grammar analysis")

	void operator()(agi::Context *c) override {
		ShowAIAnalysisSettingsDialog(c->parent);
	}
};

struct tool_text_furigana final : public Command {
	CMD_NAME("tool/text/furigana")
	STR_MENU("Japanese &Furigana Annotation...")
	STR_DISP("Japanese Furigana Annotation")
	STR_HELP("Add editable furigana annotations above or below Japanese kanji")

	void operator()(agi::Context *c) override {
		ShowJapaneseFuriganaDialog(c);
	}
};

struct tool_time_postprocess final : public Command {
	CMD_NAME("tool/time/postprocess")
	CMD_ICON(timing_processor_toolbutton)
	STR_MENU("&Timing Post-Processor...")
	STR_DISP("Timing Post-Processor")
	STR_HELP("Post-process the subtitle timing to add lead-ins and lead-outs, snap timing to scene changes, etc.")

	void operator()(agi::Context *c) override {
		ShowTimingProcessorDialog(c);
	}
};

struct tool_translation_assistant final : public Command {
	CMD_NAME("tool/translation_assistant")
	CMD_ICON(translation_toolbutton)
	STR_MENU("&Translation Assistant...")
	STR_DISP("Translation Assistant")
	STR_HELP("Open translation assistant")

	void operator()(agi::Context *c) override {
		c->videoController->Stop();
		try {
			c->dialog->ShowModal<DialogTranslation>(c);
		}
		catch (DialogTranslation::NothingToTranslate const&) {
			wxMessageBox(_("There is nothing to translate in the file."));
		}
	}
};

struct tool_translation_assistant_validator : public Command {
	CMD_TYPE(COMMAND_VALIDATE)

	bool Validate(const agi::Context *c) override {
		return !!c->dialog->Get<DialogTranslation>();
	}
};

struct tool_translation_assistant_commit final : public tool_translation_assistant_validator {
	CMD_NAME("tool/translation_assistant/commit")
	STR_MENU("&Accept changes")
	STR_DISP("Accept changes")
	STR_HELP("Commit changes and move to the next line")

	void operator()(agi::Context *c) override {
		c->dialog->Get<DialogTranslation>()->Commit(true);
	}
};

struct tool_translation_assistant_preview final : public tool_translation_assistant_validator {
	CMD_NAME("tool/translation_assistant/preview")
	STR_MENU("&Preview changes")
	STR_DISP("Preview changes")
	STR_HELP("Commit changes and stay on the current line")

	void operator()(agi::Context *c) override {
		c->dialog->Get<DialogTranslation>()->Commit(false);
	}
};

struct tool_translation_assistant_next final : public tool_translation_assistant_validator {
	CMD_NAME("tool/translation_assistant/next")
	STR_MENU("&Next Line")
	STR_DISP("Next Line")
	STR_HELP("Move to the next line without committing changes")

	void operator()(agi::Context *c) override {
		c->dialog->Get<DialogTranslation>()->NextBlock();
	}
};

struct tool_translation_assistant_prev final : public tool_translation_assistant_validator {
	CMD_NAME("tool/translation_assistant/prev")
	STR_MENU("&Previous Line")
	STR_DISP("Previous Line")
	STR_HELP("Move to the previous line without committing changes")

	void operator()(agi::Context *c) override {
		c->dialog->Get<DialogTranslation>()->PrevBlock();
	}
};

struct tool_translation_assistant_insert final : public tool_translation_assistant_validator {
	CMD_NAME("tool/translation_assistant/insert_original")
	STR_MENU("&Insert Original")
	STR_DISP("Insert Original")
	STR_HELP("Insert the untranslated text")

	void operator()(agi::Context *c) override {
		c->dialog->Get<DialogTranslation>()->InsertOriginal();
	}
};
}

namespace cmd {
	void init_tool() {
		reg(std::make_unique<tool_lyric_scroll>());
		reg(std::make_unique<tool_export>());
		reg(std::make_unique<tool_font_collector>());
		reg(std::make_unique<tool_line_select>());
		reg(std::make_unique<tool_resampleres>());
		reg(std::make_unique<tool_style_assistant>());
		reg(std::make_unique<tool_styling_assistant_commit>());
		reg(std::make_unique<tool_styling_assistant_preview>());
		reg(std::make_unique<tool_style_manager>());
		reg(std::make_unique<tool_time_kanji>());
		reg(std::make_unique<tool_time_stitch>());
		reg(std::make_unique<tool_style_overlap_check>());
		reg(std::make_unique<tool_text_cleanup>());
		reg(std::make_unique<tool_text_chinese_convert>());
		reg(std::make_unique<tool_text_pair_check>());
		reg(std::make_unique<tool_ai_analysis_settings>());
		reg(std::make_unique<tool_text_furigana>());
		reg(std::make_unique<tool_time_postprocess>());
		reg(std::make_unique<tool_translation_assistant>());
		reg(std::make_unique<tool_translation_assistant_commit>());
		reg(std::make_unique<tool_translation_assistant_preview>());
		reg(std::make_unique<tool_translation_assistant_next>());
		reg(std::make_unique<tool_translation_assistant_prev>());
		reg(std::make_unique<tool_translation_assistant_insert>());
	}
}
