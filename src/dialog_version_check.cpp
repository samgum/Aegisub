// Copyright (c) 2007, Rodrigo Braz Monteiro
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

#ifdef WITH_UPDATE_CHECKER

#include "compat.h"
#include "format.h"
#include "options.h"
#include "version.h"
#include "update_checker.h"

#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/cajun/reader.h>

#include <ctime>
#include <curl/curl.h>
#include <functional>
#include <mutex>
#include <memory>
#include <sstream>
#include <vector>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/hyperlink.h>
#include <wx/intl.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>

namespace {
std::mutex VersionCheckLock;

class VersionCheckerResultDialog final : public wxDialog {
	void OnCloseButton(wxCommandEvent &evt);
	void OnRemindMeLater(wxCommandEvent &evt);
	void OnClose(wxCloseEvent &evt);

	wxCheckBox *automatic_check_checkbox;

public:
	VersionCheckerResultDialog(wxString const& main_text, const std::vector<sgmy::updates::Release> &updates);

	bool ShouldPreventAppExit() const override { return false; }
};

VersionCheckerResultDialog::VersionCheckerResultDialog(wxString const& main_text, const std::vector<sgmy::updates::Release> &updates)
: wxDialog(nullptr, -1, _("Version Checker"))
{
	const int controls_width = 500;

	wxSizer *main_sizer = new wxBoxSizer(wxVERTICAL);

	wxStaticText *text = new wxStaticText(this, -1, main_text);
	text->Wrap(controls_width);
	main_sizer->Add(text, 0, wxBOTTOM|wxEXPAND, 6);

	for (auto const& update : updates) {
		main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND|wxALL, 6);

		text = new wxStaticText(this, -1, to_wx(update.name));
		wxFont boldfont = text->GetFont();
		boldfont.SetWeight(wxFONTWEIGHT_BOLD);
		text->SetFont(boldfont);
		main_sizer->Add(text, 0, wxEXPAND|wxBOTTOM, 6);

		wxTextCtrl *descbox = new wxTextCtrl(this, -1, to_wx(update.description), wxDefaultPosition, wxSize(controls_width,60), wxTE_MULTILINE|wxTE_READONLY);
		main_sizer->Add(descbox, 0, wxEXPAND|wxBOTTOM, 6);

		main_sizer->Add(new wxHyperlinkCtrl(this, -1, to_wx(update.url), to_wx(update.url)), 0, wxALIGN_LEFT|wxBOTTOM, 6);
	}

	automatic_check_checkbox = new wxCheckBox(this, -1, _("&Auto Check for Updates"));
	automatic_check_checkbox->SetValue(OPT_GET("App/Auto/Check For Updates")->GetBool());

	wxButton *remind_later_button = nullptr;
	if (updates.size() > 0)
		remind_later_button = new wxButton(this, wxID_NO, _("Remind me again in a &week"));

	wxButton *close_button = new wxButton(this, wxID_OK, _("&Close"));
	SetAffirmativeId(wxID_OK);
	SetEscapeId(wxID_OK);

	if (updates.size())
		main_sizer->Add(new wxStaticLine(this), 0, wxEXPAND|wxALL, 6);
	main_sizer->Add(automatic_check_checkbox, 0, wxEXPAND|wxBOTTOM, 6);

	auto button_sizer = new wxStdDialogButtonSizer();
	button_sizer->AddButton(close_button);
	if (remind_later_button)
		button_sizer->AddButton(remind_later_button);
	button_sizer->Realize();
	main_sizer->Add(button_sizer, 0, wxEXPAND, 0);

	wxSizer *outer_sizer = new wxBoxSizer(wxVERTICAL);
	outer_sizer->Add(main_sizer, 0, wxALL|wxEXPAND, 12);

	SetSizerAndFit(outer_sizer);
	Centre();
	Show();

	Bind(wxEVT_BUTTON, std::bind(&VersionCheckerResultDialog::Close, this, false), wxID_OK);
	Bind(wxEVT_BUTTON, &VersionCheckerResultDialog::OnRemindMeLater, this, wxID_NO);
	Bind(wxEVT_CLOSE_WINDOW, &VersionCheckerResultDialog::OnClose, this);
}

void VersionCheckerResultDialog::OnRemindMeLater(wxCommandEvent &) {
	// In one week
	time_t new_next_check_time = time(nullptr) + 7*24*60*60;
	OPT_SET("Version/Next Check")->SetInt(new_next_check_time);

	Close();
}

void VersionCheckerResultDialog::OnClose(wxCloseEvent &) {
	OPT_SET("App/Auto/Check For Updates")->SetBool(automatic_check_checkbox->GetValue());
	Destroy();
}

DEFINE_EXCEPTION(VersionCheckError, agi::Exception);

void PostErrorEvent(bool interactive, wxString const& error_text) {
	if (interactive) {
		agi::dispatch::Main().Async([=]{
			new VersionCheckerResultDialog(error_text, {});
		});
	}
}

size_t writeToStringCb(char *contents, size_t size, size_t nmemb, std::string *s) {
	constexpr size_t limit = 8 * 1024 * 1024;
	if (!size || nmemb > limit / size) return 0;
	size_t bytes = size * nmemb;
	if (bytes > limit - s->size()) return 0;
	try {
		s->append(contents, bytes);
		return bytes;
	}
	catch (...) {
		return 0;
	}
}

std::optional<json::UnknownElement> FetchJson(std::string const& url, bool allow_not_found = false) {
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
	if (!curl)
		throw VersionCheckError(from_wx(_("Curl could not be initialized.")));

	curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 3L);
	curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
	curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "Aegisub-SGMY update checker");

	std::string result;
	curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, writeToStringCb);
	curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &result);
	auto res_code = curl_easy_perform(curl.get());
	if (res_code != CURLE_OK)
		throw VersionCheckError(agi::format(_("Checking for updates failed: %s."), curl_easy_strerror(res_code)));

	long status = 0;
	curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
	// An empty repository has no latest release. Errors such as rate limiting
	// or a failed comparison must never be presented as "no updates".
	if (status == 404 && allow_not_found) return std::nullopt;
	if (status != 200)
		throw VersionCheckError(agi::format(_("Checking for updates failed: %s."),
			"GitHub HTTP " + std::to_string(status)));

	std::istringstream stream(result);
	json::UnknownElement root;
	try {
		json::Reader::Read(root, stream);
	}
	catch (json::Exception const& e) {
		throw VersionCheckError(agi::format(_("Checking for updates failed: %s."), e.what()));
	}
	return root;
}

void DoCheck(bool interactive) {
	std::vector<sgmy::updates::Release> results;
	std::string api = std::string(UPDATE_CHECKER_SERVER) + UPDATE_CHECKER_BASE_URL;
	if (auto response = FetchJson(api + "/releases/latest", true)) {
		try {
			if (auto release = sgmy::updates::ParseRelease(*response)) {
				std::string commit = GetBuildGitCommit();
				if (!sgmy::updates::IsBuildCommit(commit))
					throw std::runtime_error("This build has no Git commit for update comparison");
				auto comparison = FetchJson(api + "/compare/" + commit + "..." + release->tag + "?per_page=1");
				if (sgmy::updates::IsNewerRelease(*comparison))
					results.push_back(std::move(*release));
			}
		}
		catch (std::exception const& e) {
			throw VersionCheckError(agi::format(_("Checking for updates failed: %s."), e.what()));
		}
	}

	if (!results.empty() || interactive) {
		agi::dispatch::Main().Async([=]{
			wxString text;
			if (results.size() == 1)
				text = _("An update to Aegisub was found.");
			else if (results.size() > 1)
				text = _("Several possible updates to Aegisub were found.");
			else
				text = _("There are no updates to Aegisub.");

			new VersionCheckerResultDialog(text, results);
		});
	}
}
}

void PerformVersionCheck(bool interactive) {
	agi::dispatch::Background().Async([=]{
		if (!interactive) {
			// Automatic checking enabled?
			if (!OPT_GET("App/Auto/Check For Updates")->GetBool())
				return;

			// Is it actually time for a check?
			time_t next_check = OPT_GET("Version/Next Check")->GetInt();
			if (next_check > time(nullptr))
				return;
		}

		if (!VersionCheckLock.try_lock()) return;

		try {
			DoCheck(interactive);
		}
		catch (const agi::Exception &e) {
			PostErrorEvent(interactive, fmt_tl(
				"There was an error checking for updates to Aegisub:\n%s\n\nIf other applications can access the Internet fine, this is probably a temporary server problem on our end.",
				e.GetMessage()));
		}
		catch (...) {
			PostErrorEvent(interactive, _("An unknown error occurred while checking for updates to Aegisub."));
		}

		VersionCheckLock.unlock();

		agi::dispatch::Main().Async([]{
			time_t new_next_check_time = time(nullptr) + 60*60; // in one hour
			OPT_SET("Version/Next Check")->SetInt(new_next_check_time);
		});
	});
}

#endif
