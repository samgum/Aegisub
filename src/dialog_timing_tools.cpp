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
