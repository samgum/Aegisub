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
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

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
