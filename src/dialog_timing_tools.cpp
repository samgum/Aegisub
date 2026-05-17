// Copyright (c) 2026
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

#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_style.h"
#include "compat.h"
#include "include/aegisub/context.h"
#include "libresrc/libresrc.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "subs_controller.h"

#include <libaegisub/fs.h>
#include <libaegisub/io.h>
#include <libaegisub/log.h>
#include <libaegisub/path.h>
#include <libaegisub/signal.h>
#include <libaegisub/vfr.h>

#include <libaegisub/cajun/elements.h>
#include <libaegisub/cajun/reader.h>
#include <libaegisub/cajun/writer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/filename.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace {
wxString format_seconds(int ms) {
	return wxString::Format("%.3f", ms / 1000.0);
}

wxString get_history_string(json::Object& obj) {
	auto filename = to_wx(obj["filename"]);
	if (filename.empty())
		filename = _("unsaved");

	wxString anchor = obj["anchor previous"] ? _("previous line end") : _("next line start");
	wxString units = obj["use frames"] ? _("frames") : _("time");
	wxString affect = obj["selection only"] ? _("selected rows") : _("all rows");

	return wxString::Format("%s: %s, %s, %s, %s <= %.3fs",
		filename.c_str(),
		anchor.c_str(),
		units.c_str(),
		affect.c_str(),
		_("gap").c_str(),
		double((int64_t)obj["max gap ms"]) / 1000.0);
}

class DialogStitchTimings final : public wxDialog {
	agi::Context *context;
	agi::fs::path history_filename;
	json::Array history;
	agi::vfr::Framerate fps;
	agi::signal::Connection timecodes_loaded_slot;
	agi::signal::Connection selected_set_changed_slot;

	wxRadioBox *anchor_mode;
	wxRadioBox *unit_mode;
	wxRadioBox *selection_mode;
	wxTextCtrl *max_gap;
	wxListBox *history_box;

	void LoadHistory();
	void SaveHistory(int changed);
	void Process(wxCommandEvent&);
	void OnClear(wxCommandEvent&);
	void OnHistoryClick(wxCommandEvent&);
	void OnSelectedSetChanged();
	void OnTimecodesLoaded(agi::vfr::Framerate const& new_fps);
	bool ReadMaxGap(int& ms);

public:
	DialogStitchTimings(agi::Context *context);
	~DialogStitchTimings();
};

class DialogTextCleaner final : public wxDialog {
	agi::Context *context;
	agi::signal::Connection selected_set_changed_slot;

	wxCheckBox *replace_commas;
	wxCheckBox *clean_periods;
	wxCheckBox *replace_quotes;
	wxCheckBox *check_double_spaces;
	wxCheckBox *fix_double_spaces;
	wxRadioBox *selection_mode;

	void Process(wxCommandEvent&);
	void OnDoubleSpaceCheckChanged(wxCommandEvent&);
	void OnSelectedSetChanged();

public:
	DialogTextCleaner(agi::Context *context);
	~DialogTextCleaner();
};

class DialogFuriganaAnnotator final : public wxDialog {
	agi::Context *context;
	agi::fs::path readings_filename;
	agi::signal::Connection selected_set_changed_slot;

	wxRadioBox *position_mode;
	wxRadioBox *kana_mode;
	wxRadioBox *selection_mode;
	wxCheckBox *overwrite_auto_readings;
	wxCheckBox *remove_empty_annotations;
	wxSpinCtrl *size_percent;
	wxSpinCtrl *outline_percent;
	wxSpinCtrl *shadow_percent;
	wxTextCtrl *readings_text;

	void LoadReadings(std::map<std::string, std::string>& readings);
	void SaveReadings(std::map<std::string, std::string> const& readings);
	void AutoFillReadings(wxCommandEvent&);
	void RebuildReadingList(wxCommandEvent&);
	void Process(wxCommandEvent&);
	void OnSelectedSetChanged();
	std::vector<AssDialogue *> GetTargetLines() const;
	std::map<std::string, std::string> ParseReadingText() const;
	std::vector<std::string> CollectTerms() const;
	std::map<std::string, std::string> NormalizeReadings(std::map<std::string, std::string> readings) const;

public:
	DialogFuriganaAnnotator(agi::Context *context);
	~DialogFuriganaAnnotator();
};

DialogStitchTimings::DialogStitchTimings(agi::Context *context)
: wxDialog(context->parent, -1, _("Stitch Timings"))
, context(context)
, history_filename(config::path->Decode("?user/stitch_timings_history.json"))
, timecodes_loaded_slot(context->project->AddTimecodesListener(&DialogStitchTimings::OnTimecodesLoaded, this))
, selected_set_changed_slot(context->selectionController->AddSelectionListener(&DialogStitchTimings::OnSelectedSetChanged, this))
{
	SetIcons(GETICONS(shift_times_toolbutton));

	wxString anchor_vals[] = { _("Use previous line end"), _("Use next line start") };
	anchor_mode = new wxRadioBox(this, -1, _("Stitch by"), wxDefaultPosition, wxDefaultSize, 2, anchor_vals, 1);

	wxString unit_vals[] = { _("Time"), _("Frames") };
	unit_mode = new wxRadioBox(this, -1, _("Units"), wxDefaultPosition, wxDefaultSize, 2, unit_vals, 1);

	wxString selection_vals[] = { _("All rows"), _("Selected rows") };
	selection_mode = new wxRadioBox(this, -1, _("Affect"), wxDefaultPosition, wxDefaultSize, 2, selection_vals, 1);

	max_gap = new wxTextCtrl(this, -1, format_seconds(OPT_GET("Tool/Stitch Timings/Max Gap")->GetInt()));

	auto gap_sizer = new wxFlexGridSizer(2, 5, 5);
	gap_sizer->Add(new wxStaticText(this, -1, _("Maximum gap (seconds):")), wxSizerFlags().Center().Right());
	gap_sizer->Add(max_gap, wxSizerFlags(1).Expand());

	auto options_sizer = new wxBoxSizer(wxVERTICAL);
	options_sizer->Add(anchor_mode, wxSizerFlags().Expand().Border(wxBOTTOM));
	options_sizer->Add(unit_mode, wxSizerFlags().Expand().Border(wxBOTTOM));
	options_sizer->Add(selection_mode, wxSizerFlags().Expand().Border(wxBOTTOM));
	options_sizer->Add(gap_sizer, wxSizerFlags().Expand());

	auto history_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Load from history"));
	auto history_box_parent = history_sizer->GetStaticBox();
	history_box = new wxListBox(history_box_parent, -1, wxDefaultPosition, wxSize(360, 120), 0, nullptr, wxLB_HSCROLL);
	auto clear_button = new wxButton(history_box_parent, -1, _("Clear"));
	clear_button->Bind(wxEVT_BUTTON, &DialogStitchTimings::OnClear, this);
	history_sizer->Add(history_box, wxSizerFlags(1).Expand());
	history_sizer->Add(clear_button, wxSizerFlags().Expand().Border(wxTOP));

	auto top_sizer = new wxBoxSizer(wxHORIZONTAL);
	top_sizer->Add(options_sizer, wxSizerFlags().Border().Expand());
	top_sizer->Add(history_sizer, wxSizerFlags(1).Border(wxTOP | wxRIGHT | wxBOTTOM).Expand());

	auto button_sizer = CreateButtonSizer(wxOK | wxCANCEL);
	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(top_sizer, wxSizerFlags().Expand());
	main_sizer->Add(button_sizer, wxSizerFlags().Expand().Border());

	anchor_mode->SetSelection(OPT_GET("Tool/Stitch Timings/Anchor")->GetInt());
	unit_mode->SetSelection(OPT_GET("Tool/Stitch Timings/By Frames")->GetBool() ? 1 : 0);
	selection_mode->SetSelection(OPT_GET("Tool/Stitch Timings/Selection Only")->GetBool() ? 1 : 0);

	OnTimecodesLoaded(context->project->Timecodes());
	OnSelectedSetChanged();
	LoadHistory();

	SetSizerAndFit(main_sizer);
	CenterOnParent();

	Bind(wxEVT_BUTTON, &DialogStitchTimings::Process, this, wxID_OK);
	history_box->Bind(wxEVT_LISTBOX_DCLICK, &DialogStitchTimings::OnHistoryClick, this);
}

DialogStitchTimings::~DialogStitchTimings() {
	int gap = OPT_GET("Tool/Stitch Timings/Max Gap")->GetInt();
	double seconds = 0.0;
	if (max_gap->GetValue().ToDouble(&seconds) && seconds >= 0.0)
		gap = static_cast<int>(std::lround(seconds * 1000.0));
	OPT_SET("Tool/Stitch Timings/Anchor")->SetInt(anchor_mode->GetSelection());
	OPT_SET("Tool/Stitch Timings/By Frames")->SetBool(unit_mode->GetSelection() == 1);
	OPT_SET("Tool/Stitch Timings/Selection Only")->SetBool(selection_mode->GetSelection() == 1);
	OPT_SET("Tool/Stitch Timings/Max Gap")->SetInt(gap);
}

void DialogStitchTimings::OnTimecodesLoaded(agi::vfr::Framerate const& new_fps) {
	fps = new_fps;
	unit_mode->Enable(1, fps.IsLoaded());
	if (!fps.IsLoaded())
		unit_mode->SetSelection(0);
}

void DialogStitchTimings::OnSelectedSetChanged() {
	bool has_selection = !context->selectionController->GetSelectedSet().empty();
	selection_mode->Enable(1, has_selection);
	if (!has_selection)
		selection_mode->SetSelection(0);
}

bool DialogStitchTimings::ReadMaxGap(int& ms) {
	double seconds = 0.0;
	if (!max_gap->GetValue().ToDouble(&seconds) || seconds < 0.0) {
		wxMessageBox(_("Maximum gap must be a non-negative number of seconds."), _("Stitch Timings"), wxICON_EXCLAMATION);
		return false;
	}

	ms = static_cast<int>(std::lround(seconds * 1000.0));
	return true;
}

void DialogStitchTimings::OnClear(wxCommandEvent &) {
	agi::fs::Remove(history_filename);
	history_box->Clear();
	history.clear();
}

void DialogStitchTimings::OnHistoryClick(wxCommandEvent &evt) {
	size_t entry = evt.GetInt();
	if (entry >= history.size()) return;

	json::Object& obj = history[entry];
	anchor_mode->SetSelection(obj["anchor previous"] ? 0 : 1);
	unit_mode->SetSelection(obj["use frames"] && fps.IsLoaded() ? 1 : 0);
	selection_mode->SetSelection(obj["selection only"] && !context->selectionController->GetSelectedSet().empty() ? 1 : 0);
	max_gap->SetValue(format_seconds((int64_t)obj["max gap ms"]));
}

void DialogStitchTimings::SaveHistory(int changed) {
	json::Object new_entry;
	new_entry["filename"] = context->subsController->Filename().filename().string();
	new_entry["anchor previous"] = anchor_mode->GetSelection() == 0;
	new_entry["use frames"] = unit_mode->GetSelection() == 1;
	new_entry["selection only"] = selection_mode->GetSelection() == 1;
	new_entry["max gap ms"] = OPT_GET("Tool/Stitch Timings/Max Gap")->GetInt();
	new_entry["changed"] = changed;

	history.insert(history.begin(), std::move(new_entry));
	if (history.size() > 50)
		history.resize(50);

	try {
		agi::JsonWriter::Write(history, agi::io::Save(history_filename).Get());
	}
	catch (agi::fs::FileSystemError const& e) {
		LOG_E("dialog_timing_tools/save_history") << "Cannot save stitch timings history: " << e.GetMessage();
	}
}

void DialogStitchTimings::LoadHistory() {
	history_box->Clear();
	history_box->Freeze();

	try {
		json::UnknownElement root;
		json::Reader::Read(root, *agi::io::Open(history_filename));
		history = std::move(static_cast<json::Array&>(root));

		for (auto& history_entry : history)
			history_box->Append(get_history_string(history_entry));
	}
	catch (agi::fs::FileSystemError const& e) {
		LOG_D("dialog_timing_tools/load_history") << "Cannot load stitch timings history: " << e.GetMessage();
	}
	catch (json::Exception const& e) {
		LOG_D("dialog_timing_tools/load_history") << "Cannot load stitch timings history: " << e.what();
	}
	catch (...) {
		history_box->Thaw();
		throw;
	}

	history_box->Thaw();
}

void DialogStitchTimings::Process(wxCommandEvent &) {
	int max_gap_ms = 0;
	if (!ReadMaxGap(max_gap_ms))
		return;

	bool anchor_previous = anchor_mode->GetSelection() == 0;
	bool use_frames = unit_mode->GetSelection() == 1 && fps.IsLoaded();
	bool selection_only = selection_mode->GetSelection() == 1;
	auto const& selection = context->selectionController->GetSelectedSet();

	std::vector<AssDialogue *> lines;
	for (auto& line : context->ass->Events) {
		if (line.Comment)
			continue;
		if (selection_only && !selection.count(&line))
			continue;
		lines.push_back(&line);
	}

	int changed = 0;
	for (size_t i = 1; i < lines.size(); ++i) {
		auto prev = lines[i - 1];
		auto next = lines[i];
		int gap = next->Start - prev->End;
		if (gap <= 0 || gap > max_gap_ms)
			continue;

		if (use_frames) {
			int prev_end = fps.FrameAtTime(prev->End, agi::vfr::END);
			int next_start = fps.FrameAtTime(next->Start, agi::vfr::START);
			if (next_start <= prev_end + 1)
				continue;

			if (anchor_previous)
				next->Start = fps.TimeAtFrame(prev_end + 1, agi::vfr::START);
			else
				prev->End = fps.TimeAtFrame(next_start - 1, agi::vfr::END);
		}
		else {
			if (anchor_previous)
				next->Start = prev->End;
			else
				prev->End = next->Start;
		}

		++changed;
	}

	OPT_SET("Tool/Stitch Timings/Max Gap")->SetInt(max_gap_ms);

	if (changed) {
		context->ass->Commit(_("stitch timings"), AssFile::COMMIT_DIAG_TIME);
		SaveHistory(changed);
	}
	else {
		wxMessageBox(_("No subtitle gaps matched the current settings."), _("Stitch Timings"), wxICON_INFORMATION);
	}

	Close();
}

DialogTextCleaner::DialogTextCleaner(agi::Context *context)
: wxDialog(context->parent, -1, _("Subtitle Text Cleanup"))
, context(context)
, selected_set_changed_slot(context->selectionController->AddSelectionListener(&DialogTextCleaner::OnSelectedSetChanged, this))
{
	replace_commas = new wxCheckBox(this, -1, _("Replace Chinese commas with spaces"));
	clean_periods = new wxCheckBox(this, -1, _("Remove Chinese periods; use a space when text follows"));
	replace_quotes = new wxCheckBox(this, -1, _("Replace Chinese double quotes with English quotes"));
	check_double_spaces = new wxCheckBox(this, -1, _("Check for consecutive spaces"));
	fix_double_spaces = new wxCheckBox(this, -1, _("Replace consecutive spaces with one space"));

	replace_commas->SetValue(OPT_GET("Tool/Text Cleanup/Replace Commas")->GetBool());
	clean_periods->SetValue(OPT_GET("Tool/Text Cleanup/Clean Periods")->GetBool());
	replace_quotes->SetValue(OPT_GET("Tool/Text Cleanup/Replace Quotes")->GetBool());
	check_double_spaces->SetValue(OPT_GET("Tool/Text Cleanup/Check Double Spaces")->GetBool());
	fix_double_spaces->SetValue(OPT_GET("Tool/Text Cleanup/Fix Double Spaces")->GetBool());

	wxString selection_vals[] = { _("All rows"), _("Selected rows") };
	selection_mode = new wxRadioBox(this, -1, _("Apply to"), wxDefaultPosition, wxDefaultSize, 2, selection_vals);
	selection_mode->SetSelection(OPT_GET("Tool/Text Cleanup/Selection Only")->GetBool() ? 1 : 0);

	auto operations_sizer = new wxStaticBoxSizer(wxVERTICAL, this, _("Operations"));
	operations_sizer->Add(replace_commas, wxSizerFlags().Expand().Border(wxBOTTOM, 4));
	operations_sizer->Add(clean_periods, wxSizerFlags().Expand().Border(wxBOTTOM, 4));
	operations_sizer->Add(replace_quotes, wxSizerFlags().Expand().Border(wxBOTTOM, 4));
	operations_sizer->Add(check_double_spaces, wxSizerFlags().Expand().Border(wxBOTTOM, 4));
	operations_sizer->Add(fix_double_spaces, wxSizerFlags().Expand());

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(operations_sizer, wxSizerFlags().Expand().Border());
	main_sizer->Add(selection_mode, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	main_sizer->Add(CreateButtonSizer(wxOK | wxCANCEL), wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));

	SetSizerAndFit(main_sizer);
	CenterOnParent();

	Bind(wxEVT_BUTTON, &DialogTextCleaner::Process, this, wxID_OK);
	check_double_spaces->Bind(wxEVT_CHECKBOX, &DialogTextCleaner::OnDoubleSpaceCheckChanged, this);
	wxCommandEvent evt;
	OnDoubleSpaceCheckChanged(evt);
	OnSelectedSetChanged();
}

DialogTextCleaner::~DialogTextCleaner() {
	OPT_SET("Tool/Text Cleanup/Replace Commas")->SetBool(replace_commas->GetValue());
	OPT_SET("Tool/Text Cleanup/Clean Periods")->SetBool(clean_periods->GetValue());
	OPT_SET("Tool/Text Cleanup/Replace Quotes")->SetBool(replace_quotes->GetValue());
	OPT_SET("Tool/Text Cleanup/Check Double Spaces")->SetBool(check_double_spaces->GetValue());
	OPT_SET("Tool/Text Cleanup/Fix Double Spaces")->SetBool(fix_double_spaces->GetValue());
	OPT_SET("Tool/Text Cleanup/Selection Only")->SetBool(selection_mode->GetSelection() == 1);
}

void DialogTextCleaner::OnDoubleSpaceCheckChanged(wxCommandEvent&) {
	fix_double_spaces->Enable(check_double_spaces->GetValue());
	if (!check_double_spaces->GetValue())
		fix_double_spaces->SetValue(false);
}

void DialogTextCleaner::OnSelectedSetChanged() {
	bool has_selection = !context->selectionController->GetSelectedSet().empty();
	selection_mode->Enable(1, has_selection);
	if (!has_selection)
		selection_mode->SetSelection(0);
}

struct TextCleanupStats {
	size_t checked_lines = 0;
	size_t changed_lines = 0;

	size_t comma_lines = 0;
	size_t comma_replacements = 0;

	size_t period_removed_lines = 0;
	size_t period_spaced_lines = 0;
	size_t periods_removed = 0;
	size_t periods_spaced = 0;

	size_t quote_lines = 0;
	size_t quote_replacements = 0;

	size_t double_space_lines = 0;
	size_t double_space_runs = 0;
	size_t double_space_fixed_lines = 0;
	size_t double_space_runs_fixed = 0;
};

size_t replace_all(std::string& text, std::string const& from, std::string const& to) {
	size_t count = 0;
	size_t pos = 0;
	while ((pos = text.find(from, pos)) != std::string::npos) {
		text.replace(pos, from.size(), to);
		pos += to.size();
		++count;
	}
	return count;
}

enum class FollowingText {
	None,
	StartsWithSpace,
	StartsWithoutSpace
};

FollowingText following_plain_text(std::vector<std::unique_ptr<AssDialogueBlock>> const& blocks, size_t block_index) {
	for (size_t i = block_index + 1; i < blocks.size(); ++i) {
		if (blocks[i]->GetType() != AssBlockType::PLAIN)
			continue;

		auto const& text = static_cast<AssDialogueBlockPlain const *>(blocks[i].get())->text;
		if (!text.empty())
			return text[0] == ' ' ? FollowingText::StartsWithSpace : FollowingText::StartsWithoutSpace;
	}

	return FollowingText::None;
}

size_t clean_fullwidth_periods(std::string& text, FollowingText following_text, size_t& spaced) {
	static std::string const period = "\xE3\x80\x82";
	size_t removed = 0;
	spaced = 0;

	size_t pos = 0;
	while ((pos = text.find(period, pos)) != std::string::npos) {
		size_t next = pos + period.size();
		if ((next < text.size() && text[next] != ' ') || (next == text.size() && following_text == FollowingText::StartsWithoutSpace)) {
			text.replace(pos, period.size(), " ");
			++spaced;
			pos += 1;
		}
		else {
			text.erase(pos, period.size());
			++removed;
		}
	}

	return removed;
}

size_t count_double_space_runs(std::string const& text) {
	size_t runs = 0;
	for (size_t i = 0; i + 1 < text.size(); ++i) {
		if (text[i] == ' ' && text[i + 1] == ' ') {
			++runs;
			while (i + 1 < text.size() && text[i + 1] == ' ')
				++i;
		}
	}
	return runs;
}

size_t collapse_double_space_runs(std::string& text) {
	size_t runs = 0;
	size_t write = 0;
	bool previous_space = false;

	for (size_t read = 0; read < text.size(); ++read) {
		if (text[read] == ' ') {
			if (previous_space) {
				if (read == 1 || text[read - 2] != ' ')
					++runs;
				continue;
			}
			previous_space = true;
		}
		else {
			previous_space = false;
		}

		text[write++] = text[read];
	}

	text.resize(write);
	return runs;
}

void DialogTextCleaner::Process(wxCommandEvent&) {
	bool do_commas = replace_commas->GetValue();
	bool do_periods = clean_periods->GetValue();
	bool do_quotes = replace_quotes->GetValue();
	bool do_check_double_spaces = check_double_spaces->GetValue();
	bool do_fix_double_spaces = do_check_double_spaces && fix_double_spaces->GetValue();

	if (!do_commas && !do_periods && !do_quotes && !do_check_double_spaces) {
		wxMessageBox(_("No cleanup operations are selected."), _("Subtitle Text Cleanup"), wxICON_EXCLAMATION);
		return;
	}

	std::vector<AssDialogue *> lines;
	if (selection_mode->GetSelection() == 1)
		lines = context->selectionController->GetSortedSelection();
	else {
		for (auto& line : context->ass->Events)
			lines.push_back(&line);
	}

	TextCleanupStats stats;
	static std::string const fullwidth_comma = "\xEF\xBC\x8C";
	static std::string const left_quote = "\xE2\x80\x9C";
	static std::string const right_quote = "\xE2\x80\x9D";

	for (auto line : lines) {
		if (line->Comment)
			continue;

		++stats.checked_lines;

		auto blocks = line->ParseTags();
		bool line_changed = false;
		bool line_has_commas = false;
		bool line_removed_periods = false;
		bool line_spaced_periods = false;
		bool line_has_quotes = false;
		bool line_has_double_spaces = false;
		bool line_fixed_double_spaces = false;

		for (size_t block_index = 0; block_index < blocks.size(); ++block_index) {
			auto& block = blocks[block_index];
			if (block->GetType() != AssBlockType::PLAIN)
				continue;

			auto& text = static_cast<AssDialogueBlockPlain *>(block.get())->text;

			if (do_commas) {
				size_t count = replace_all(text, fullwidth_comma, " ");
				if (count) {
					stats.comma_replacements += count;
					line_has_commas = true;
					line_changed = true;
				}
			}

			if (do_periods) {
				size_t spaced = 0;
				size_t removed = clean_fullwidth_periods(text, following_plain_text(blocks, block_index), spaced);
				if (removed || spaced) {
					stats.periods_removed += removed;
					stats.periods_spaced += spaced;
					line_removed_periods = line_removed_periods || removed != 0;
					line_spaced_periods = line_spaced_periods || spaced != 0;
					line_changed = true;
				}
			}

			if (do_quotes) {
				size_t count = replace_all(text, left_quote, "\"");
				count += replace_all(text, right_quote, "\"");
				if (count) {
					stats.quote_replacements += count;
					line_has_quotes = true;
					line_changed = true;
				}
			}

			if (do_check_double_spaces) {
				size_t runs = count_double_space_runs(text);
				if (runs) {
					stats.double_space_runs += runs;
					line_has_double_spaces = true;

					if (do_fix_double_spaces) {
						size_t fixed = collapse_double_space_runs(text);
						stats.double_space_runs_fixed += fixed;
						line_fixed_double_spaces = line_fixed_double_spaces || fixed != 0;
						line_changed = line_changed || fixed != 0;
					}
				}
			}
		}

		if (line_has_commas) ++stats.comma_lines;
		if (line_removed_periods) ++stats.period_removed_lines;
		if (line_spaced_periods) ++stats.period_spaced_lines;
		if (line_has_quotes) ++stats.quote_lines;
		if (line_has_double_spaces) ++stats.double_space_lines;
		if (line_fixed_double_spaces) ++stats.double_space_fixed_lines;

		if (line_changed) {
			line->UpdateText(blocks);
			++stats.changed_lines;
		}
	}

	if (stats.changed_lines)
		context->ass->Commit(_("clean subtitle text"), AssFile::COMMIT_DIAG_TEXT);

	wxString report;
	report += _("Subtitle text cleanup");
	report += "\n\n";
	report += wxString::Format("%s: %llu\n", _("Checked non-comment dialogue lines").c_str(), static_cast<unsigned long long>(stats.checked_lines));
	report += wxString::Format("%s: %llu\n\n", _("Changed lines").c_str(), static_cast<unsigned long long>(stats.changed_lines));
	report += wxString::Format("%s: %llu (%s: %llu)\n", _("Chinese comma replacements").c_str(), static_cast<unsigned long long>(stats.comma_replacements), _("lines involved").c_str(), static_cast<unsigned long long>(stats.comma_lines));
	report += wxString::Format("%s: %llu (%s: %llu)\n", _("Chinese periods removed").c_str(), static_cast<unsigned long long>(stats.periods_removed), _("lines involved").c_str(), static_cast<unsigned long long>(stats.period_removed_lines));
	report += wxString::Format("%s: %llu (%s: %llu)\n", _("Chinese periods converted to spaces").c_str(), static_cast<unsigned long long>(stats.periods_spaced), _("lines involved").c_str(), static_cast<unsigned long long>(stats.period_spaced_lines));
	report += wxString::Format("%s: %llu (%s: %llu)\n", _("Chinese quote replacements").c_str(), static_cast<unsigned long long>(stats.quote_replacements), _("lines involved").c_str(), static_cast<unsigned long long>(stats.quote_lines));
	report += wxString::Format("%s: %llu (%s: %llu)\n", _("Consecutive space groups found").c_str(), static_cast<unsigned long long>(stats.double_space_runs), _("lines involved").c_str(), static_cast<unsigned long long>(stats.double_space_lines));
	report += wxString::Format("%s: %llu (%s: %llu)\n", _("Consecutive space groups replaced").c_str(), static_cast<unsigned long long>(stats.double_space_runs_fixed), _("lines involved").c_str(), static_cast<unsigned long long>(stats.double_space_fixed_lines));

	wxMessageBox(report, _("Subtitle Text Cleanup"), wxICON_INFORMATION);
	Close();
}

namespace {
static std::string const furigana_marker = "{sgmy-furigana}";

bool read_utf8_codepoint(std::string const& text, size_t pos, uint32_t& codepoint, size_t& length) {
	unsigned char c = static_cast<unsigned char>(text[pos]);
	if (c < 0x80) {
		codepoint = c;
		length = 1;
		return true;
	}
	if ((c & 0xE0) == 0xC0 && pos + 1 < text.size()) {
		codepoint = ((c & 0x1F) << 6) | (static_cast<unsigned char>(text[pos + 1]) & 0x3F);
		length = 2;
		return true;
	}
	if ((c & 0xF0) == 0xE0 && pos + 2 < text.size()) {
		codepoint = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(text[pos + 2]) & 0x3F);
		length = 3;
		return true;
	}
	if ((c & 0xF8) == 0xF0 && pos + 3 < text.size()) {
		codepoint = ((c & 0x07) << 18) | ((static_cast<unsigned char>(text[pos + 1]) & 0x3F) << 12) | ((static_cast<unsigned char>(text[pos + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(text[pos + 3]) & 0x3F);
		length = 4;
		return true;
	}

	codepoint = c;
	length = 1;
	return false;
}

void append_utf8_codepoint(std::string& text, uint32_t cp) {
	if (cp <= 0x7F) {
		text += static_cast<char>(cp);
	}
	else if (cp <= 0x7FF) {
		text += static_cast<char>(0xC0 | (cp >> 6));
		text += static_cast<char>(0x80 | (cp & 0x3F));
	}
	else if (cp <= 0xFFFF) {
		text += static_cast<char>(0xE0 | (cp >> 12));
		text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		text += static_cast<char>(0x80 | (cp & 0x3F));
	}
	else {
		text += static_cast<char>(0xF0 | (cp >> 18));
		text += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
		text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
		text += static_cast<char>(0x80 | (cp & 0x3F));
	}
}

std::string convert_kana(std::string const& text, bool katakana) {
	std::string converted;
	for (size_t pos = 0; pos < text.size(); ) {
		uint32_t cp = 0;
		size_t len = 1;
		read_utf8_codepoint(text, pos, cp, len);

		if (katakana && cp >= 0x3041 && cp <= 0x3096)
			cp += 0x60;
		else if (!katakana && cp >= 0x30A1 && cp <= 0x30F6)
			cp -= 0x60;

		append_utf8_codepoint(converted, cp);
		pos += len;
	}
	return converted;
}

std::vector<std::string> split_ass_visual_lines(std::string const& text) {
	std::vector<std::string> lines;
	size_t start = 0;
	for (size_t pos = 0; pos < text.size(); ) {
		if (text[pos] == '\\' && pos + 1 < text.size() && (text[pos + 1] == 'N' || text[pos + 1] == 'n')) {
			lines.push_back(text.substr(start, pos - start));
			pos += 2;
			start = pos;
		}
		else {
			++pos;
		}
	}

	lines.push_back(text.substr(start));
	return lines;
}

std::string join_ass_visual_lines(std::vector<std::string> const& lines) {
	std::string joined;
	for (size_t i = 0; i < lines.size(); ++i) {
		if (i)
			joined += "\\N";
		joined += lines[i];
	}
	return joined;
}

bool is_cjk_ideograph(uint32_t cp) {
	return (cp >= 0x3400 && cp <= 0x4DBF) ||
		(cp >= 0x4E00 && cp <= 0x9FFF) ||
		(cp >= 0xF900 && cp <= 0xFAFF) ||
		(cp >= 0x20000 && cp <= 0x2A6DF) ||
		(cp >= 0x2A700 && cp <= 0x2B73F) ||
		(cp >= 0x2B740 && cp <= 0x2B81F) ||
		(cp >= 0x2B820 && cp <= 0x2CEAF);
}

std::string strip_existing_furigana(std::string const& text, bool& had_furigana) {
	had_furigana = false;

	auto lines = split_ass_visual_lines(text);
	std::vector<std::string> base_lines;
	base_lines.reserve(lines.size());

	for (auto const& line : lines) {
		if (line.starts_with(furigana_marker)) {
			had_furigana = true;
			continue;
		}

		base_lines.push_back(line);
	}

	if (had_furigana)
		return join_ass_visual_lines(base_lines);

	return text;
}

void collect_kanji_terms_from_text(std::string const& raw_text, std::vector<std::string>& terms, std::set<std::string>& seen) {
	bool had_furigana = false;
	AssDialogue line;
	line.Text = strip_existing_furigana(raw_text, had_furigana);

	auto blocks = line.ParseTags();
	for (auto& block : blocks) {
		if (block->GetType() != AssBlockType::PLAIN)
			continue;

		auto const& text = static_cast<AssDialogueBlockPlain const *>(block.get())->text;
		for (size_t pos = 0; pos < text.size(); ) {
			uint32_t cp = 0;
			size_t len = 1;
			read_utf8_codepoint(text, pos, cp, len);

			if (!is_cjk_ideograph(cp)) {
				pos += len;
				continue;
			}

			size_t start = pos;
			do {
				pos += len;
				if (pos >= text.size())
					break;
				read_utf8_codepoint(text, pos, cp, len);
			} while (is_cjk_ideograph(cp));

			auto term = text.substr(start, pos - start);
			if (seen.insert(term).second)
				terms.push_back(term);
		}
	}
}

bool has_kanji(std::string const& text) {
	for (size_t pos = 0; pos < text.size(); ) {
		uint32_t cp = 0;
		size_t len = 1;
		read_utf8_codepoint(text, pos, cp, len);
		if (is_cjk_ideograph(cp))
			return true;
		pos += len;
	}
	return false;
}

std::string format_ass_number(double value);

struct FuriganaFormat {
	std::string tags;
	double base_size = 48.0;
	double ruby_size = 18.0;
};

double furigana_display_units(std::string const& text) {
	double units = 0.0;
	for (size_t pos = 0; pos < text.size(); ) {
		uint32_t cp = 0;
		size_t len = 1;
		read_utf8_codepoint(text, pos, cp, len);

		if (cp == ' ' || cp == '\t')
			units += 0.5;
		else if (cp < 0x80)
			units += 0.55;
		else if ((cp >= 0x3040 && cp <= 0x30FF) || is_cjk_ideograph(cp) || (cp >= 0xFF01 && cp <= 0xFF60))
			units += 1.0;
		else
			units += 0.8;

		pos += len;
	}
	return units;
}

bool is_karaoke_tag(AssOverrideTag const& tag) {
	return tag.Name == "\\k" || tag.Name == "\\K" || tag.Name == "\\kf" || tag.Name == "\\ko";
}

bool resets_style_tag(AssOverrideTag const& tag) {
	return tag.Name == "\\r";
}

void append_sanitized_override_state(AssDialogueBlockOverride const& block, std::string& active_tags) {
	for (auto const& tag : block.Tags) {
		if (is_karaoke_tag(tag))
			continue;

		auto tag_text = static_cast<std::string>(tag);
		if (resets_style_tag(tag))
			active_tags = tag_text;
		else
			active_tags += tag_text;
	}
}

std::string hidden_state_tags(std::string const& active_tags, FuriganaFormat const& format) {
	if (active_tags.starts_with("\\r"))
		return "{" + active_tags + format.tags + "\\alpha&HFF&}";
	return "{\\r" + active_tags + format.tags + "\\alpha&HFF&}";
}

void append_ideographic_spaces(std::string& text, size_t count) {
	for (size_t i = 0; i < count; ++i)
		text += "\xE3\x80\x80";
}

void append_hidden_space(std::string& text, double width, FuriganaFormat const& format) {
	size_t count = static_cast<size_t>(std::max(0.0, std::round(width / std::max(1.0, format.ruby_size))));
	append_ideographic_spaces(text, count);
}

std::string make_visible_furigana_segment(std::string const& target, std::string const& reading, FuriganaFormat const& format, std::string const& active_tags) {
	double target_width = std::max(0.1, furigana_display_units(target)) * format.base_size;
	double reading_width = std::max(0.1, furigana_display_units(reading)) * format.ruby_size;
	double pad = std::max(0.0, (target_width - reading_width) / 2.0);

	std::string result;
	append_hidden_space(result, pad, format);
	result += "{\\alpha&H00&" + format.tags + "}" +
		reading +
		hidden_state_tags(active_tags, format);
	append_hidden_space(result, pad, format);
	return result;
}

std::string make_aligned_reading_for_cjk_run(std::string const& run, std::map<std::string, std::string> const& readings, FuriganaFormat const& format, std::string const& active_tags, bool& any_reading) {
	std::vector<size_t> boundaries;
	std::vector<uint32_t> codepoints;
	for (size_t pos = 0; pos < run.size(); ) {
		uint32_t cp = 0;
		size_t len = 1;
		read_utf8_codepoint(run, pos, cp, len);
		boundaries.push_back(pos);
		codepoints.push_back(cp);
		pos += len;
	}
	boundaries.push_back(run.size());

	std::string result;
	for (size_t i = 0; i < codepoints.size(); ) {
		bool matched = false;
		for (size_t end = codepoints.size(); end > i; --end) {
			auto term = run.substr(boundaries[i], boundaries[end] - boundaries[i]);
			auto it = readings.find(term);
			if (it != readings.end() && !it->second.empty()) {
				result += make_visible_furigana_segment(term, it->second, format, active_tags);
				i = end;
				matched = true;
				any_reading = true;
				break;
			}
		}

		if (!matched) {
			append_hidden_space(result, furigana_display_units(run.substr(boundaries[i], boundaries[i + 1] - boundaries[i])) * format.base_size, format);
			++i;
		}
	}

	return result;
}

void append_hidden_text_width(std::string& result, std::string const& text, FuriganaFormat const& format) {
	append_hidden_space(result, furigana_display_units(text) * format.base_size, format);
}

std::string make_aligned_reading_line(std::string const& raw_text, std::map<std::string, std::string> const& readings, FuriganaFormat const& format, bool& any_reading) {
	bool had_furigana = false;
	AssDialogue line;
	line.Text = strip_existing_furigana(raw_text, had_furigana);

	std::string result = hidden_state_tags("", format);
	std::string active_tags;
	auto blocks = line.ParseTags();
	for (auto& block : blocks) {
		if (block->GetType() == AssBlockType::OVERRIDE) {
			append_sanitized_override_state(*static_cast<AssDialogueBlockOverride const *>(block.get()), active_tags);
			result += hidden_state_tags(active_tags, format);
			continue;
		}

		if (block->GetType() != AssBlockType::PLAIN) {
			append_hidden_text_width(result, block->GetText(), format);
			continue;
		}

		auto const& text = static_cast<AssDialogueBlockPlain const *>(block.get())->text;
		for (size_t pos = 0; pos < text.size(); ) {
			uint32_t cp = 0;
			size_t len = 1;
			read_utf8_codepoint(text, pos, cp, len);

			if (!is_cjk_ideograph(cp)) {
				append_hidden_text_width(result, text.substr(pos, len), format);
				pos += len;
				continue;
			}

			size_t start = pos;
			do {
				pos += len;
				if (pos >= text.size())
					break;
				read_utf8_codepoint(text, pos, cp, len);
			} while (is_cjk_ideograph(cp));

			result += make_aligned_reading_for_cjk_run(text.substr(start, pos - start), readings, format, active_tags, any_reading);
		}
	}

	return result + "{\\r}";
}

std::string format_ass_number(double value) {
	std::ostringstream ss;
	ss.setf(std::ios::fixed, std::ios::floatfield);
	ss.precision(2);
	ss << value;
	auto ret = ss.str();
	while (ret.size() > 1 && ret.back() == '0')
		ret.pop_back();
	if (!ret.empty() && ret.back() == '.')
		ret.pop_back();
	return ret;
}

FuriganaFormat make_furigana_format(AssStyle const* style, int size_percent, int outline_percent, int shadow_percent) {
	double base_size = style ? style->fontsize : 48.0;
	double base_outline = style ? style->outline_w : 2.0;
	double base_shadow = style ? style->shadow_w : 2.0;

	double ruby_size = std::max(1.0, base_size * size_percent / 100.0);
	double ruby_outline = std::max(0.0, base_outline * outline_percent / 100.0);
	double ruby_shadow = std::max(0.0, base_shadow * shadow_percent / 100.0);

	return {
		"\\fs" + format_ass_number(ruby_size) +
		"\\bord" + format_ass_number(ruby_outline) +
		"\\shad" + format_ass_number(ruby_shadow) +
		"\\fscx100\\fscy100\\fsp0\\p0",
		base_size,
		ruby_size
	};
}

std::string compose_furigana_text(std::string const& base, std::map<std::string, std::string> const& readings, FuriganaFormat const& format, bool above, bool& any_reading) {
	std::vector<std::string> output_lines;
	any_reading = false;

	for (auto const& base_line : split_ass_visual_lines(base)) {
		bool line_has_reading = false;
		auto reading_line = make_aligned_reading_line(base_line, readings, format, line_has_reading);
		if (line_has_reading) {
			auto ruby = furigana_marker + reading_line;
			if (above) {
				output_lines.push_back(ruby);
				output_lines.push_back(base_line);
			}
			else {
				output_lines.push_back(base_line);
				output_lines.push_back(ruby);
			}
			any_reading = true;
		}
		else {
			output_lines.push_back(base_line);
		}
	}

	return join_ass_visual_lines(output_lines);
}

std::string json_escape(std::string const& text) {
	std::string out;
	for (unsigned char c : text) {
		switch (c) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[7];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			}
			else
				out += static_cast<char>(c);
		}
	}
	return out;
}

std::string make_json_array(std::vector<std::string> const& terms) {
	std::string out = "[";
	for (size_t i = 0; i < terms.size(); ++i) {
		if (i)
			out += ",";
		out += "\"" + json_escape(terms[i]) + "\"";
	}
	out += "]";
	return out;
}

std::string plain_text_for_furigana(std::string const& raw_text) {
	bool had_furigana = false;
	AssDialogue line;
	line.Text = strip_existing_furigana(raw_text, had_furigana);

	std::string plain;
	auto blocks = line.ParseTags();
	for (auto& block : blocks) {
		if (block->GetType() == AssBlockType::PLAIN)
			plain += static_cast<AssDialogueBlockPlain const *>(block.get())->text;
	}
	return plain;
}

std::string make_sudachi_input_json(std::vector<std::string> const& terms, std::vector<std::string> const& texts) {
	return "{\"terms\":" + make_json_array(terms) + ",\"texts\":" + make_json_array(texts) + "}";
}

std::string sudachi_script() {
	return R"PYTHON(
import json
import sys

try:
    from sudachipy import dictionary
    from sudachipy import tokenizer
except Exception as exc:
    print("IMPORT_ERROR: " + str(exc), file=sys.stderr)
    sys.exit(10)

def kata_to_hira(text):
    out = []
    for ch in text:
        cp = ord(ch)
        if 0x30A1 <= cp <= 0x30F6:
            out.append(chr(cp - 0x60))
        else:
            out.append(ch)
    return "".join(out)

def has_kanji(text):
    return any(
        0x3400 <= ord(ch) <= 0x4DBF or
        0x4E00 <= ord(ch) <= 0x9FFF or
        0xF900 <= ord(ch) <= 0xFAFF or
        0x20000 <= ord(ch) <= 0x2CEAF
        for ch in text
    )

def is_kanji(ch):
    cp = ord(ch)
    return (
        0x3400 <= cp <= 0x4DBF or
        0x4E00 <= cp <= 0x9FFF or
        0xF900 <= cp <= 0xFAFF or
        0x20000 <= cp <= 0x2CEAF
    )

def kana_surface(text):
    return kata_to_hira(text)

def add_surface_readings(surface, reading, readings):
    if not surface or not reading or not has_kanji(surface):
        return

    hira_surface = kana_surface(surface)
    hira_reading = kata_to_hira(reading)
    pos = 0
    while pos < len(surface):
        if not is_kanji(surface[pos]):
            pos += 1
            continue
        start = pos
        while pos < len(surface) and is_kanji(surface[pos]):
            pos += 1

        kanji = surface[start:pos]
        prefix = hira_surface[:start]
        suffix = hira_surface[pos:]
        ruby = hira_reading

        if prefix and ruby.startswith(prefix):
            ruby = ruby[len(prefix):]
        if suffix and ruby.endswith(suffix):
            ruby = ruby[:-len(suffix)]

        if ruby and ruby != kanji:
            readings.setdefault(kanji, ruby)

try:
    data = json.load(open(sys.argv[1], "r", encoding="utf-8-sig"))
    if isinstance(data, list):
        terms = data
        texts = []
    else:
        terms = data.get("terms", [])
        texts = data.get("texts", [])

    tok = dictionary.Dictionary().create()
    mode = tokenizer.Tokenizer.SplitMode.A
    readings = {}

    for text in texts:
        for morpheme in tok.tokenize(text, mode):
            add_surface_readings(morpheme.surface(), morpheme.reading_form(), readings)

    for term in terms:
        if term in readings:
            continue
        reading = "".join(m.reading_form() for m in tok.tokenize(term, mode))
        if reading and reading != "*" and reading != term:
            readings[term] = kata_to_hira(reading)

    json.dump(readings, open(sys.argv[2], "w", encoding="utf-8"), ensure_ascii=False)
except Exception as exc:
    print("SUDACHI_ERROR: " + str(exc), file=sys.stderr)
    sys.exit(11)
)PYTHON";
}
}

DialogFuriganaAnnotator::DialogFuriganaAnnotator(agi::Context *context)
: wxDialog(context->parent, -1, _("Japanese Furigana Annotation"), wxDefaultPosition, wxSize(760, 640))
, context(context)
, readings_filename(config::path->Decode("?user/furigana_readings.json"))
, selected_set_changed_slot(context->selectionController->AddSelectionListener(&DialogFuriganaAnnotator::OnSelectedSetChanged, this))
{
	wxString position_vals[] = { _("Above kanji"), _("Below kanji") };
	position_mode = new wxRadioBox(this, -1, _("Annotation position"), wxDefaultPosition, wxDefaultSize, 2, position_vals);
	position_mode->SetMinSize(FromDIP(wxSize(180, 78)));
	position_mode->SetSelection(OPT_GET("Tool/Furigana Annotation/Position")->GetInt());

	wxString kana_vals[] = { _("Hiragana"), _("Katakana") };
	kana_mode = new wxRadioBox(this, -1, _("Reading script"), wxDefaultPosition, wxDefaultSize, 2, kana_vals);
	kana_mode->SetMinSize(FromDIP(wxSize(180, 78)));
	kana_mode->SetSelection(OPT_GET("Tool/Furigana Annotation/Kana Mode")->GetInt());

	wxString selection_vals[] = { _("All rows"), _("Selected rows") };
	selection_mode = new wxRadioBox(this, -1, _("Apply to"), wxDefaultPosition, wxDefaultSize, 2, selection_vals);
	selection_mode->SetMinSize(FromDIP(wxSize(180, 78)));
	selection_mode->SetSelection(OPT_GET("Tool/Furigana Annotation/Selection Only")->GetBool() ? 1 : 0);

	overwrite_auto_readings = new wxCheckBox(this, -1, _("Overwrite existing readings when auto filling"));
	overwrite_auto_readings->SetValue(OPT_GET("Tool/Furigana Annotation/Overwrite Auto Readings")->GetBool());
	remove_empty_annotations = new wxCheckBox(this, -1, _("Remove existing annotations when readings are empty"));
	remove_empty_annotations->SetValue(OPT_GET("Tool/Furigana Annotation/Remove Empty Annotations")->GetBool());

	int saved_size_percent = OPT_GET("Tool/Furigana Annotation/Size Percent")->GetInt();
	if (!OPT_GET("Tool/Furigana Annotation/Size Default Migrated")->GetBool()) {
		if (saved_size_percent == 50)
			saved_size_percent = 35;
		OPT_SET("Tool/Furigana Annotation/Size Default Migrated")->SetBool(true);
	}
	size_percent = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 10, 100, saved_size_percent);
	outline_percent = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 100, OPT_GET("Tool/Furigana Annotation/Outline Percent")->GetInt());
	shadow_percent = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0, 100, OPT_GET("Tool/Furigana Annotation/Shadow Percent")->GetInt());

	auto options = new wxFlexGridSizer(2, 5, 12);
	options->AddGrowableCol(1);
	options->Add(new wxStaticText(this, -1, _("Furigana size (% of style):")), wxSizerFlags().Center().Left());
	options->Add(size_percent, wxSizerFlags().Expand());
	options->Add(new wxStaticText(this, -1, _("Outline size (% of style):")), wxSizerFlags().Center().Left());
	options->Add(outline_percent, wxSizerFlags().Expand());
	options->Add(new wxStaticText(this, -1, _("Shadow size (% of style):")), wxSizerFlags().Center().Left());
	options->Add(shadow_percent, wxSizerFlags().Expand());
	options->Add(overwrite_auto_readings, wxSizerFlags().Expand());
	options->Add(remove_empty_annotations, wxSizerFlags().Expand());

	auto readings_box = new wxStaticBoxSizer(wxVERTICAL, this, _("Readings"));
	readings_text = new wxTextCtrl(readings_box->GetStaticBox(), -1, "", wxDefaultPosition, FromDIP(wxSize(700, 280)), wxTE_MULTILINE | wxTE_DONTWRAP);
	readings_box->Add(new wxStaticText(readings_box->GetStaticBox(), -1, _("Edit one entry per line as kanji=reading. Empty readings are skipped.")), wxSizerFlags().Expand().Border(wxBOTTOM, 4));
	readings_box->Add(readings_text, wxSizerFlags(1).Expand());
	auto auto_fill_button = new wxButton(readings_box->GetStaticBox(), -1, _("Auto fill readings with SudachiPy"));
	readings_box->Add(auto_fill_button, wxSizerFlags().Expand().Border(wxTOP, 4));
	auto refresh_button = new wxButton(readings_box->GetStaticBox(), -1, _("Refresh kanji list"));
	readings_box->Add(refresh_button, wxSizerFlags().Expand().Border(wxTOP, 4));

	auto top = new wxBoxSizer(wxHORIZONTAL);
	top->Add(position_mode, wxSizerFlags(1).Expand().Border());
	top->Add(kana_mode, wxSizerFlags(1).Expand().Border(wxTOP | wxRIGHT | wxBOTTOM));
	top->Add(selection_mode, wxSizerFlags(1).Expand().Border(wxTOP | wxRIGHT | wxBOTTOM));

	auto main_sizer = new wxBoxSizer(wxVERTICAL);
	main_sizer->Add(top, wxSizerFlags().Expand());
	main_sizer->Add(options, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	main_sizer->Add(readings_box, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	main_sizer->Add(CreateButtonSizer(wxOK | wxCANCEL), wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));

	SetSizer(main_sizer);
	SetMinSize(FromDIP(wxSize(720, 600)));
	main_sizer->Fit(this);
	CenterOnParent();

	Bind(wxEVT_BUTTON, &DialogFuriganaAnnotator::Process, this, wxID_OK);
	selection_mode->Bind(wxEVT_RADIOBOX, &DialogFuriganaAnnotator::RebuildReadingList, this);
	auto_fill_button->Bind(wxEVT_BUTTON, &DialogFuriganaAnnotator::AutoFillReadings, this);
	refresh_button->Bind(wxEVT_BUTTON, &DialogFuriganaAnnotator::RebuildReadingList, this);

	OnSelectedSetChanged();
	wxCommandEvent evt;
	RebuildReadingList(evt);
}

DialogFuriganaAnnotator::~DialogFuriganaAnnotator() {
	OPT_SET("Tool/Furigana Annotation/Position")->SetInt(position_mode->GetSelection());
	OPT_SET("Tool/Furigana Annotation/Kana Mode")->SetInt(kana_mode->GetSelection());
	OPT_SET("Tool/Furigana Annotation/Selection Only")->SetBool(selection_mode->GetSelection() == 1);
	OPT_SET("Tool/Furigana Annotation/Overwrite Auto Readings")->SetBool(overwrite_auto_readings->GetValue());
	OPT_SET("Tool/Furigana Annotation/Remove Empty Annotations")->SetBool(remove_empty_annotations->GetValue());
	OPT_SET("Tool/Furigana Annotation/Size Percent")->SetInt(size_percent->GetValue());
	OPT_SET("Tool/Furigana Annotation/Outline Percent")->SetInt(outline_percent->GetValue());
	OPT_SET("Tool/Furigana Annotation/Shadow Percent")->SetInt(shadow_percent->GetValue());
}

void DialogFuriganaAnnotator::LoadReadings(std::map<std::string, std::string>& readings) {
	try {
		json::UnknownElement root;
		json::Reader::Read(root, *agi::io::Open(readings_filename));
		for (auto& entry : static_cast<json::Object&>(root))
			readings[entry.first] = static_cast<std::string>(entry.second);
	}
	catch (...) {
	}
}

void DialogFuriganaAnnotator::SaveReadings(std::map<std::string, std::string> const& readings) {
	json::Object root;
	for (auto const& [kanji, reading] : readings) {
		if (!reading.empty())
			root[kanji] = reading;
	}

	try {
		agi::JsonWriter::Write(root, agi::io::Save(readings_filename).Get());
	}
	catch (agi::Exception const& err) {
		LOG_E("furigana/save") << err.GetMessage();
	}
}

std::vector<AssDialogue *> DialogFuriganaAnnotator::GetTargetLines() const {
	std::vector<AssDialogue *> lines;
	if (selection_mode->GetSelection() == 1)
		lines = context->selectionController->GetSortedSelection();
	else {
		for (auto& line : context->ass->Events)
			lines.push_back(&line);
	}
	return lines;
}

std::vector<std::string> DialogFuriganaAnnotator::CollectTerms() const {
	std::vector<std::string> terms;
	std::set<std::string> seen;
	for (auto line : GetTargetLines()) {
		if (!line->Comment)
			collect_kanji_terms_from_text(line->Text.get(), terms, seen);
	}
	return terms;
}

std::map<std::string, std::string> DialogFuriganaAnnotator::ParseReadingText() const {
	std::map<std::string, std::string> readings;
	auto lines = wxSplit(readings_text->GetValue(), '\n', '\0');
	for (auto const& line : lines) {
		auto trimmed = line;
		trimmed.Trim(true).Trim(false);
		if (trimmed.empty())
			continue;

		auto eq = trimmed.Find('=');
		if (eq == wxNOT_FOUND)
			continue;

		auto kanji = trimmed.Left(eq);
		auto reading = trimmed.Mid(eq + 1);
		kanji.Trim(true).Trim(false);
		reading.Trim(true).Trim(false);
		if (!kanji.empty())
			readings[from_wx(kanji)] = from_wx(reading);
	}
	return readings;
}

std::map<std::string, std::string> DialogFuriganaAnnotator::NormalizeReadings(std::map<std::string, std::string> readings) const {
	bool katakana = kana_mode->GetSelection() == 1;
	for (auto& [kanji, reading] : readings)
		reading = convert_kana(reading, katakana);
	return readings;
}

void DialogFuriganaAnnotator::RebuildReadingList(wxCommandEvent&) {
	std::map<std::string, std::string> readings;
	LoadReadings(readings);
	auto edited_readings = ParseReadingText();
	for (auto const& [kanji, reading] : edited_readings)
		readings[kanji] = reading;

	auto terms = CollectTerms();
	std::vector<std::string> texts;
	for (auto line : GetTargetLines()) {
		if (!line->Comment) {
			auto plain = plain_text_for_furigana(line->Text.get());
			if (!plain.empty())
				texts.push_back(plain);
		}
	}

	wxString text;
	std::set<std::string> displayed;
	for (auto const& term : terms) {
		displayed.insert(term);
		text += to_wx(term);
		text += "=";
		auto it = readings.find(term);
		if (it != readings.end())
			text += to_wx(convert_kana(it->second, kana_mode->GetSelection() == 1));
		text += "\n";
	}
	for (auto const& [term, reading] : readings) {
		if (reading.empty() || !has_kanji(term) || displayed.count(term))
			continue;

		bool used = false;
		for (auto const& text_line : texts) {
			if (text_line.find(term) != std::string::npos) {
				used = true;
				break;
			}
		}
		if (!used)
			continue;

		text += to_wx(term);
		text += "=";
		text += to_wx(convert_kana(reading, kana_mode->GetSelection() == 1));
		text += "\n";
	}

	readings_text->SetValue(text);
}

void DialogFuriganaAnnotator::AutoFillReadings(wxCommandEvent&) {
	auto terms = CollectTerms();
	if (terms.empty()) {
		wxMessageBox(_("No kanji terms were found in the current range."), _("Japanese Furigana Annotation"), wxICON_INFORMATION);
		return;
	}

	std::map<std::string, std::string> readings;
	LoadReadings(readings);
	auto edited_readings = ParseReadingText();
	for (auto const& [kanji, reading] : edited_readings)
		readings[kanji] = reading;
	std::vector<std::string> texts;
	for (auto line : GetTargetLines()) {
		if (!line->Comment) {
			auto plain = plain_text_for_furigana(line->Text.get());
			if (!plain.empty())
				texts.push_back(plain);
		}
	}

	wxString script_path = wxFileName::CreateTempFileName("aegisub_furigana_script");
	wxString terms_path = wxFileName::CreateTempFileName("aegisub_furigana_terms");
	wxString output_path = wxFileName::CreateTempFileName("aegisub_furigana_output");

	auto cleanup = [&] {
		try {
			agi::fs::Remove(agi::fs::path(from_wx(script_path)));
			agi::fs::Remove(agi::fs::path(from_wx(terms_path)));
			agi::fs::Remove(agi::fs::path(from_wx(output_path)));
		}
		catch (...) {
		}
	};

	try {
		agi::io::Save(agi::fs::path(from_wx(script_path))).Get() << sudachi_script();
		agi::io::Save(agi::fs::path(from_wx(terms_path))).Get() << make_sudachi_input_json(terms, texts);

		wxArrayString output;
		wxArrayString errors;
		wxString command = wxString::Format("python \"%s\" \"%s\" \"%s\"", script_path, terms_path, output_path);
		long code = wxExecute(command, output, errors, wxEXEC_SYNC);

		if (code != 0) {
			output.Clear();
			errors.Clear();
			command = wxString::Format("python3 \"%s\" \"%s\" \"%s\"", script_path, terms_path, output_path);
			code = wxExecute(command, output, errors, wxEXEC_SYNC);
		}

#ifdef __WXMSW__
		if (code != 0) {
			output.Clear();
			errors.Clear();
			command = wxString::Format("py -3 \"%s\" \"%s\" \"%s\"", script_path, terms_path, output_path);
			code = wxExecute(command, output, errors, wxEXEC_SYNC);
		}
#endif

		if (code != 0) {
			cleanup();
			wxString error_text;
			for (auto const& line : errors)
				error_text += line + "\n";
			if (error_text.empty()) {
				for (auto const& line : output)
					error_text += line + "\n";
			}

			wxMessageBox(
				_("SudachiPy is not available or failed to generate readings.") + "\n\n" +
				_("Install it with:") + "\n" +
				"python -m pip install --upgrade sudachipy sudachidict_core\n\n" +
				error_text,
				_("Japanese Furigana Annotation"),
				wxICON_EXCLAMATION);
			return;
		}

		json::UnknownElement root;
		json::Reader::Read(root, *agi::io::Open(agi::fs::path(from_wx(output_path))));
		size_t filled = 0;
		size_t preserved = 0;
		bool overwrite = overwrite_auto_readings->GetValue();
		bool katakana = kana_mode->GetSelection() == 1;
		for (auto& entry : static_cast<json::Object&>(root)) {
			auto reading = convert_kana(static_cast<std::string>(entry.second), katakana);
			if (reading.empty())
				continue;

			if (!overwrite && !readings[entry.first].empty()) {
				++preserved;
				continue;
			}

			if (readings[entry.first] != reading) {
				readings[entry.first] = reading;
				++filled;
			}
		}

		wxString text;
		std::set<std::string> displayed;
		for (auto const& term : terms) {
			displayed.insert(term);
			text += to_wx(term);
			text += "=";
			auto it = readings.find(term);
			if (it != readings.end())
				text += to_wx(convert_kana(it->second, katakana));
			text += "\n";
		}
		for (auto const& [term, reading] : readings) {
			if (!reading.empty() && has_kanji(term) && !displayed.count(term)) {
				text += to_wx(term);
				text += "=";
				text += to_wx(convert_kana(reading, katakana));
				text += "\n";
			}
		}
		readings_text->SetValue(text);
		SaveReadings(readings);
		cleanup();

		wxString report;
		report += wxString::Format("%s: %llu\n", _("Auto-filled reading entries").c_str(), static_cast<unsigned long long>(filled));
		report += wxString::Format("%s: %llu", _("Preserved existing reading entries").c_str(), static_cast<unsigned long long>(preserved));
		wxMessageBox(report, _("Japanese Furigana Annotation"), wxICON_INFORMATION);
	}
	catch (agi::Exception const& err) {
		cleanup();
		wxMessageBox(to_wx(err.GetMessage()), _("Japanese Furigana Annotation"), wxICON_EXCLAMATION);
	}
	catch (std::exception const& err) {
		cleanup();
		wxMessageBox(to_wx(err.what()), _("Japanese Furigana Annotation"), wxICON_EXCLAMATION);
	}
}

void DialogFuriganaAnnotator::OnSelectedSetChanged() {
	bool has_selection = !context->selectionController->GetSelectedSet().empty();
	selection_mode->Enable(1, has_selection);
	if (!has_selection)
		selection_mode->SetSelection(0);
}

void DialogFuriganaAnnotator::Process(wxCommandEvent&) {
	auto readings = NormalizeReadings(ParseReadingText());
	size_t nonempty_readings = 0;
	for (auto const& [kanji, reading] : readings) {
		if (!reading.empty())
			++nonempty_readings;
	}

	if (!nonempty_readings && !remove_empty_annotations->GetValue()) {
		wxMessageBox(_("No kanji readings are listed."), _("Japanese Furigana Annotation"), wxICON_EXCLAMATION);
		return;
	}

	size_t checked_lines = 0;
	size_t changed_lines = 0;
	size_t annotated_lines = 0;
	size_t removed_lines = 0;
	bool above = position_mode->GetSelection() == 0;

	for (auto line : GetTargetLines()) {
		if (line->Comment)
			continue;

		++checked_lines;
		bool had_furigana = false;
		auto base_text = strip_existing_furigana(line->Text.get(), had_furigana);

		std::string new_text = base_text;
		bool has_reading = false;
		auto style = context->ass->GetStyle(line->Style.get());
		auto format = make_furigana_format(style, size_percent->GetValue(), outline_percent->GetValue(), shadow_percent->GetValue());
		auto annotated_text = compose_furigana_text(base_text, readings, format, above, has_reading);
		if (has_reading) {
			new_text = annotated_text;
			++annotated_lines;
		}
		else if (had_furigana && remove_empty_annotations->GetValue()) {
			++removed_lines;
		}
		else {
			continue;
		}

		if (line->Text.get() != new_text) {
			line->Text = new_text;
			++changed_lines;
		}
	}

	if (changed_lines)
		context->ass->Commit(_("add furigana annotations"), AssFile::COMMIT_DIAG_TEXT);

	SaveReadings(readings);

	wxString report;
	report += _("Japanese furigana annotation");
	report += "\n\n";
	report += wxString::Format("%s: %llu\n", _("Checked non-comment dialogue lines").c_str(), static_cast<unsigned long long>(checked_lines));
	report += wxString::Format("%s: %llu\n", _("Changed lines").c_str(), static_cast<unsigned long long>(changed_lines));
	report += wxString::Format("%s: %llu\n", _("Annotated lines").c_str(), static_cast<unsigned long long>(annotated_lines));
	report += wxString::Format("%s: %llu\n", _("Removed annotation lines").c_str(), static_cast<unsigned long long>(removed_lines));
	report += wxString::Format("%s: %llu\n", _("Reading entries").c_str(), static_cast<unsigned long long>(nonempty_readings));

	wxMessageBox(report, _("Japanese Furigana Annotation"), wxICON_INFORMATION);
	Close();
}

wxString format_pair(AssDialogue const* a, AssDialogue const* b) {
	return wxString::Format("%d-%d", a->Row + 1, b->Row + 1);
}

void add_example(std::vector<wxString>& examples, AssDialogue const* a, AssDialogue const* b) {
	if (examples.size() < 8)
		examples.push_back(format_pair(a, b));
}

wxString join_examples(std::vector<wxString> const& examples) {
	wxString joined;
	for (size_t i = 0; i < examples.size(); ++i) {
		if (i)
			joined += ", ";
		joined += examples[i];
	}
	return joined;
}

struct OverlapStats {
	size_t lines = 0;
	long long exact_pairs = 0;
	long long same_start_pairs = 0;
	long long same_end_pairs = 0;
	long long crossing_pairs = 0;
	std::set<int> exact_lines;
	std::set<int> same_start_lines;
	std::set<int> same_end_lines;
	std::set<int> crossing_lines;
	std::vector<wxString> examples;
};

void add_lines(std::set<int>& rows, AssDialogue const* a, AssDialogue const* b) {
	rows.insert(a->Row + 1);
	rows.insert(b->Row + 1);
}

wxString BuildOverlapReport(agi::Context *context) {
	std::map<std::string, std::vector<AssDialogue *>> styles;
	for (auto& line : context->ass->Events) {
		if (!line.Comment)
			styles[line.Style.get()].push_back(&line);
	}

	wxString report;
	report += _("Style overlap check");
	report += "\n";
	report += _("Comment lines are ignored. Counts are pairs within the same style; touching endpoints are not counted as crossing overlaps.");
	report += "\n\n";

	bool any_issue = false;
	for (auto& [style, lines] : styles) {
		std::sort(lines.begin(), lines.end(), [](AssDialogue const* a, AssDialogue const* b) {
			if (a->Start != b->Start) return a->Start < b->Start;
			if (a->End != b->End) return a->End < b->End;
			return a->Row < b->Row;
		});

		OverlapStats stats;
		stats.lines = lines.size();
		for (size_t i = 0; i < lines.size(); ++i) {
			for (size_t j = i + 1; j < lines.size(); ++j) {
				auto a = lines[i];
				auto b = lines[j];

				bool exact = a->Start == b->Start && a->End == b->End;
				bool same_start = a->Start == b->Start;
				bool same_end = a->End == b->End;
				bool crossing = a->Start < b->End && b->Start < a->End;

				if (exact) {
					++stats.exact_pairs;
					add_lines(stats.exact_lines, a, b);
				}
				if (same_start) {
					++stats.same_start_pairs;
					add_lines(stats.same_start_lines, a, b);
				}
				if (same_end) {
					++stats.same_end_pairs;
					add_lines(stats.same_end_lines, a, b);
				}
				if (crossing) {
					++stats.crossing_pairs;
					add_lines(stats.crossing_lines, a, b);
					add_example(stats.examples, a, b);
				}
			}
		}

		any_issue |= stats.exact_pairs || stats.same_start_pairs || stats.same_end_pairs || stats.crossing_pairs;

		wxString style_name = to_wx(style);
		report += wxString::Format("[%s] %llu %s\n", style_name.c_str(), static_cast<unsigned long long>(stats.lines), _("lines").c_str());
		report += wxString::Format("  %s: %lld (%s: %llu)\n", _("Exact same time pairs").c_str(), stats.exact_pairs, _("lines involved").c_str(), static_cast<unsigned long long>(stats.exact_lines.size()));
		report += wxString::Format("  %s: %lld (%s: %llu)\n", _("Same start time pairs").c_str(), stats.same_start_pairs, _("lines involved").c_str(), static_cast<unsigned long long>(stats.same_start_lines.size()));
		report += wxString::Format("  %s: %lld (%s: %llu)\n", _("Same end time pairs").c_str(), stats.same_end_pairs, _("lines involved").c_str(), static_cast<unsigned long long>(stats.same_end_lines.size()));
		report += wxString::Format("  %s: %lld (%s: %llu)\n", _("Crossing overlap pairs").c_str(), stats.crossing_pairs, _("lines involved").c_str(), static_cast<unsigned long long>(stats.crossing_lines.size()));
		if (!stats.examples.empty())
			report += wxString::Format("  %s: %s\n", _("First crossing row pairs").c_str(), join_examples(stats.examples).c_str());
		report += "\n";
	}

	if (styles.empty())
		report += _("No non-comment dialogue lines were found.");
	else if (!any_issue)
		report += _("No same-style timing overlaps were found.");

	return report;
}
}

void ShowStitchTimingsDialog(agi::Context *c) {
	DialogStitchTimings(c).ShowModal();
}

void ShowStyleOverlapCheckDialog(agi::Context *c) {
	wxDialog d(c->parent, -1, _("Check Style Overlaps"), wxDefaultPosition, wxSize(700, 500));
	auto report = new wxTextCtrl(&d, -1, BuildOverlapReport(c), wxDefaultPosition, wxSize(680, 420), wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);

	auto sizer = new wxBoxSizer(wxVERTICAL);
	sizer->Add(report, wxSizerFlags(1).Expand().Border());
	sizer->Add(d.CreateButtonSizer(wxOK), wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
	d.SetSizer(sizer);
	d.CenterOnParent();
	d.ShowModal();
}

void ShowSubtitleTextCleanupDialog(agi::Context *c) {
	DialogTextCleaner(c).ShowModal();
}

void ShowJapaneseFuriganaDialog(agi::Context *c) {
	DialogFuriganaAnnotator(c).ShowModal();
}
