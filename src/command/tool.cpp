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
	bool animate = true;
	int center_x = 0;
	int center_y = 0;
	int line_gap = 0;
	int active_size = 165;
	int inactive_size = 128;
	int visible_lines = 3;
	int transition_ms = 700;
	int margin_lr = 120;
	int active_alpha = 0;
	int inactive_alpha = 88;
	int layer = 4;
	int wrap_after = 0;
	std::string active_color = "&HFFFFFF&";
	std::string inactive_color = "&HD8D8D8&";
};

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
	settings.animate = OPT_GET("Tool/Lyric Scroll/Animate")->GetBool();
	settings.center_x = opt_int("Tool/Lyric Scroll/Center X", 0, 10000);
	settings.center_y = opt_int("Tool/Lyric Scroll/Center Y", 0, 10000);
	settings.line_gap = opt_int("Tool/Lyric Scroll/Line Gap", 0, 1000);
	settings.active_size = opt_int("Tool/Lyric Scroll/Active Size", 6, 400);
	settings.inactive_size = opt_int("Tool/Lyric Scroll/Inactive Size", 6, 400);
	settings.visible_lines = opt_int("Tool/Lyric Scroll/Visible Lines", 0, 8);
	settings.transition_ms = opt_int("Tool/Lyric Scroll/Transition MS", 0, 5000);
	settings.margin_lr = opt_int("Tool/Lyric Scroll/Margin LR", 0, 3000);
	settings.active_alpha = opt_int("Tool/Lyric Scroll/Active Alpha", 0, 255);
	settings.inactive_alpha = opt_int("Tool/Lyric Scroll/Inactive Alpha", 0, 255);
	settings.layer = opt_int("Tool/Lyric Scroll/Layer", 0, 999);
	settings.wrap_after = opt_int("Tool/Lyric Scroll/Wrap After", 0, 200);
	settings.active_color = opt_ass_color("Tool/Lyric Scroll/Active Color", "&HFFFFFF&");
	settings.inactive_color = opt_ass_color("Tool/Lyric Scroll/Inactive Color", "&HD8D8D8&");

	if (settings.center_x == 1920 && settings.center_y == 1120 && settings.line_gap == 150 &&
		settings.active_size == 88 && settings.inactive_size == 62 && settings.visible_lines == 2 &&
		settings.transition_ms == 360 && settings.margin_lr == 220 && settings.layer == 20 &&
		settings.wrap_after == 46 && settings.inactive_color == "&HA8A8A8&")
	{
		settings.center_x = 0;
		settings.center_y = 0;
		settings.line_gap = 0;
		settings.active_size = 165;
		settings.inactive_size = 128;
		settings.visible_lines = 3;
		settings.transition_ms = 700;
		settings.margin_lr = 120;
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
	OPT_SET("Tool/Lyric Scroll/Animate")->SetBool(settings.animate);
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
	set_opt_ass_color("Tool/Lyric Scroll/Active Color", settings.active_color);
	set_opt_ass_color("Tool/Lyric Scroll/Inactive Color", settings.inactive_color);
}

class DialogLyricScroll final : public wxDialog {
	wxRadioBox *scope = nullptr;
	wxChoice *source_action = nullptr;
	wxCheckBox *clear_previous = nullptr;
	wxCheckBox *strip_tags = nullptr;
	wxCheckBox *animate = nullptr;
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
	wxTextCtrl *active_color = nullptr;
	wxTextCtrl *inactive_color = nullptr;
	LyricScrollSettings settings;

	wxSpinCtrl *spin(wxWindow *parent, int value, int min_value, int max_value) {
		return new wxSpinCtrl(parent, -1, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, min_value, max_value, value);
	}

	void add_row(wxWindow *parent, wxFlexGridSizer *grid, wxString const& label, wxWindow *ctrl) {
		grid->Add(new wxStaticText(parent, -1, label), wxSizerFlags().Center().Right());
		grid->Add(ctrl, wxSizerFlags(1).Expand());
	}

	void OnOK(wxCommandEvent&) {
		settings.scope = scope->GetSelection();
		settings.source_action = source_action->GetSelection();
		settings.clear_previous = clear_previous->GetValue();
		settings.strip_tags = strip_tags->GetValue();
		settings.animate = animate->GetValue();
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
		settings.active_color = normalize_ass_color(from_wx(active_color->GetValue()), "&HFFFFFF&");
		settings.inactive_color = normalize_ass_color(from_wx(inactive_color->GetValue()), "&HA8A8A8&");
		save_lyric_scroll_settings(settings);
		EndModal(wxID_OK);
	}

public:
	DialogLyricScroll(wxWindow *parent, agi::Context *c)
	: wxDialog(parent, -1, _("滚动歌词生成器"))
	, settings(load_lyric_scroll_settings())
	{
		int res_x = 0;
		int res_y = 0;
		c->ass->GetResolution(res_x, res_y);
		if (settings.center_x == 0) settings.center_x = res_x > 0 ? res_x / 2 : 960;
		if (settings.center_y == 0) settings.center_y = res_y > 0 ? res_y / 2 : 540;

		auto notebook = new wxNotebook(this, -1);
		auto common_page = new wxPanel(notebook);
		auto advanced_page = new wxPanel(notebook);

		wxString scope_choices[] = { _("仅选中行"), _("全部对白行") };
		scope = new wxRadioBox(common_page, -1, _("作用范围"), wxDefaultPosition, wxDefaultSize, 2, scope_choices, 1, wxRA_SPECIFY_COLS);
		scope->SetSelection(settings.scope);

		source_action = new wxChoice(common_page, -1);
		source_action->Append(_("保留原字幕"));
		source_action->Append(_("注释隐藏原字幕（推荐）"));
		source_action->Append(_("删除原字幕"));
		source_action->SetSelection(settings.source_action);

		clear_previous = new wxCheckBox(advanced_page, -1, _("重新生成前清理上一轮滚动歌词"));
		clear_previous->SetValue(settings.clear_previous);
		strip_tags = new wxCheckBox(advanced_page, -1, _("清理原字幕里的特效标签"));
		strip_tags->SetValue(settings.strip_tags);
		animate = new wxCheckBox(advanced_page, -1, _("启用平滑滚动"));
		animate->SetValue(settings.animate);

		center_x = spin(advanced_page, settings.center_x, 0, 10000);
		center_y = spin(common_page, settings.center_y, 0, 10000);
		line_gap = spin(common_page, settings.line_gap, 0, 1000);
		active_size = spin(common_page, settings.active_size, 6, 400);
		inactive_size = spin(common_page, settings.inactive_size, 6, 400);
		visible_lines = spin(common_page, settings.visible_lines, 0, 8);
		transition_ms = spin(common_page, settings.transition_ms, 0, 5000);
		margin_lr = spin(advanced_page, settings.margin_lr, 0, 3000);
		active_alpha = spin(advanced_page, settings.active_alpha, 0, 255);
		inactive_alpha = spin(advanced_page, settings.inactive_alpha, 0, 255);
		layer = spin(advanced_page, settings.layer, 0, 999);
		wrap_after = spin(common_page, settings.wrap_after, 0, 200);
		active_color = new wxTextCtrl(advanced_page, -1, to_wx(settings.active_color));
		inactive_color = new wxTextCtrl(advanced_page, -1, to_wx(settings.inactive_color));

		auto common_grid = new wxFlexGridSizer(2, 8, 8);
		common_grid->AddGrowableCol(1, 1);
		add_row(common_page, common_grid, _("原字幕处理"), source_action);
		add_row(common_page, common_grid, _("当前歌词 Y 位置"), center_y);
		add_row(common_page, common_grid, _("当前行字号"), active_size);
		add_row(common_page, common_grid, _("上下行字号"), inactive_size);
		add_row(common_page, common_grid, _("行距"), line_gap);
		add_row(common_page, common_grid, _("上下显示行数"), visible_lines);
		add_row(common_page, common_grid, _("滚动时长（毫秒）"), transition_ms);
		add_row(common_page, common_grid, _("长歌词换行字数"), wrap_after);

		auto common_sizer = new wxBoxSizer(wxVERTICAL);
		common_sizer->Add(scope, wxSizerFlags().Expand().Border());
		common_sizer->Add(common_grid, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		common_page->SetSizer(common_sizer);

		auto advanced_grid = new wxFlexGridSizer(2, 8, 8);
		advanced_grid->AddGrowableCol(1, 1);
		add_row(advanced_page, advanced_grid, _("中心 X 位置"), center_x);
		add_row(advanced_page, advanced_grid, _("左右边距"), margin_lr);
		add_row(advanced_page, advanced_grid, _("当前行透明度"), active_alpha);
		add_row(advanced_page, advanced_grid, _("上下行透明度"), inactive_alpha);
		add_row(advanced_page, advanced_grid, _("图层"), layer);
		add_row(advanced_page, advanced_grid, _("当前行颜色"), active_color);
		add_row(advanced_page, advanced_grid, _("上下行颜色"), inactive_color);

		auto advanced_sizer = new wxBoxSizer(wxVERTICAL);
		advanced_sizer->Add(advanced_grid, wxSizerFlags(1).Expand().Border());
		advanced_sizer->Add(clear_previous, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		advanced_sizer->Add(strip_tags, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		advanced_sizer->Add(animate, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		advanced_page->SetSizer(advanced_sizer);

		notebook->AddPage(common_page, _("常用设置"), true);
		notebook->AddPage(advanced_page, _("高级设置"));

		auto main = new wxBoxSizer(wxVERTICAL);
		main->Add(notebook, wxSizerFlags(1).Expand().Border());
		main->Add(CreateButtonSizer(wxOK | wxCANCEL), wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		SetSizerAndFit(main);
		SetMinSize(wxSize(540, -1));
		CenterOnParent();
		Bind(wxEVT_BUTTON, &DialogLyricScroll::OnOK, this, wxID_OK);
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

std::string plain_lyric_text(AssDialogue *line, LyricScrollSettings const& settings) {
	std::string text = settings.strip_tags ? line->GetStrippedText() : clean_motion_conflicts(line->Text.get());
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
	std::snprintf(buffer, sizeof buffer, "&H00%02X%02X%02X", gray, gray, gray);
	return buffer;
}

std::vector<std::tuple<int, int, double>> split_transition(int start, int end, int motion_steps) {
	int duration = end - start;
	if (duration <= 0)
		return {};

	int steps = std::max(1, std::min(motion_steps, std::max(1, duration / 15)));
	std::vector<std::tuple<int, int, double>> spans;
	int previous = start;
	for (int i = 0; i < steps; ++i) {
		int current = start + static_cast<int>(std::lround(duration * (i + 1.0) / steps));
		if (current <= previous)
			continue;

		double progress = ease_out_cubic((i + 0.5) / steps);
		spans.emplace_back(previous, current, progress);
		previous = current;
	}
	return spans;
}

void upsert_scroll_style(AssFile *ass, std::string const& name, std::string const& font, int size, std::string const& primary, int margin, int outline, int shadow) {
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
	style->outline = outline ? agi::Color("&H00111111") : agi::Color("&H00000000");
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
	style->alignment = 5;
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

std::string build_body(LyricRow const& row, bool current, LyricScrollSettings const& settings) {
	std::string primary = escape_ass_text(row.primary);
	std::string secondary = escape_ass_text(row.secondary);
	if (current) {
		std::string body = "{\\an5\\blur0.5\\bord0\\shad0\\fsp0}" + primary;
		if (!secondary.empty()) {
			int secondary_size = std::max(6, static_cast<int>(std::lround(settings.active_size * 72.0 / 165.0)));
			body += "\\N{\\fs" + std::to_string(secondary_size) + "\\alpha&H42&\\bord0\\shad0}" + secondary;
		}
		return body;
	}

	std::string body = "{\\an5\\blur1\\bord0\\shad0\\fsp0}" + primary;
	if (!secondary.empty()) {
		int secondary_size = std::max(6, static_cast<int>(std::lround(settings.inactive_size * 58.0 / 128.0)));
		body += "\\N{\\fs" + std::to_string(secondary_size) + "\\alpha&H78&\\bord0\\shad0}" + secondary;
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

AssDialogue *add_position_dialogue(agi::Context *c, LyricRow const& row, int layer, int start, int end, double y, double scale, int alpha, std::string const& color, bool current, int x, LyricScrollSettings const& settings) {
	char buffer[256];
	std::snprintf(buffer, sizeof buffer, "{\\an5\\pos(%d,%d)\\alpha&H%02X&\\1c%s\\fscx%.1f\\fscy%.1f}",
		x, static_cast<int>(std::lround(y)), std::max(0, std::min(255, alpha)), color.c_str(), scale, scale);
	return add_scroll_dialogue(c, row.line, layer, start, end, current ? "ScrollCurrent" : "ScrollDim", std::string(buffer) + build_body(row, current, settings));
}

void generate_scroll_events(agi::Context *c, std::vector<LyricRow> const& rows, std::vector<CreditRow> const& credits, LyricScrollSettings const& settings, Selection& new_selection, AssDialogue *&new_active) {
	int width = 0;
	int height = 0;
	c->ass->GetResolution(width, height);
	if (width <= 0) width = 3840;
	if (height <= 0) height = 2160;

	int x = settings.center_x > 0 ? settings.center_x : width / 2;
	int center = settings.center_y > 0 ? settings.center_y : height / 2;
	int gap = settings.line_gap > 0 ? settings.line_gap : static_cast<int>(std::lround(height * 0.118));
	int window = settings.visible_lines;
	int motion_steps = 28;

	for (auto const& credit : credits) {
		std::string text = escape_ass_text(replace_all(credit.text, "\\N", " / "));
		auto generated = add_scroll_dialogue(c, credit.line, settings.layer + 2, credit.start, credit.end, "ScrollCredit",
			"{\\an5\\pos(" + std::to_string(x) + "," + std::to_string(static_cast<int>(std::lround(height * 0.176))) + ")\\alpha&H20&}" + text);
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
			double progress;
			std::tie(span_start, span_end, progress) = span;
			for (int row_index = transition_start_row; row_index < transition_end_row; ++row_index) {
				int target_offset = row_index - static_cast<int>(active_index);
				int previous_offset = row_index - (static_cast<int>(active_index) - 1);
				if (std::abs(target_offset) > window)
					continue;

				int start_offset = std::abs(previous_offset) <= window ? previous_offset : target_offset + 1;
				double float_offset = lerp(start_offset, target_offset, progress);
				bool current = target_offset == 0;
				auto generated = add_position_dialogue(c, rows[row_index], current ? settings.layer : std::max(0, settings.layer - 1),
					span_start, span_end, center + float_offset * gap, scale_for_offset(float_offset),
					alpha_for_offset(float_offset, settings), color_for_offset(float_offset), current, x, settings);
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
				hold_start, end, center + offset * gap, scale_for_offset(offset),
				alpha_for_offset(offset, settings), color_for_offset(offset), current, x, settings);
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

	std::string primary_font = style_font(c->ass.get(), primary_style, "思源黑体 CN Heavy");
	std::string secondary_font = style_font(c->ass.get(), secondary_style, "思源黑体 CN Medium");
	upsert_scroll_style(c->ass.get(), "ScrollCurrent", primary_font, settings.active_size, settings.active_color, settings.margin_lr, 0, 0);
	upsert_scroll_style(c->ass.get(), "ScrollDim", secondary_font.empty() ? primary_font : secondary_font, settings.inactive_size, settings.inactive_color, settings.margin_lr, 0, 0);
	upsert_scroll_style(c->ass.get(), "ScrollCredit", primary_font, 120, settings.active_color, settings.margin_lr, 2, 2);
	generate_scroll_events(c, rows, credits, settings, new_selection, new_active);

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
	STR_MENU("滚动歌词生成器(&L)...")
	STR_DISP("滚动歌词生成器")
	STR_HELP("生成音乐软件式逐行滚动歌词字幕")

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
