// Copyright (c) 2026
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.

#include "ass_dialogue.h"
#include "compat.h"
#include "dialogs.h"
#include "include/aegisub/context.h"
#include "options.h"
#include "selection_controller.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/dialog.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#ifdef WITH_UPDATE_CHECKER
#include <curl/curl.h>
#endif

namespace {
std::string session_api_key;
std::map<std::string, std::string> response_cache;

struct ProviderPreset {
	const char *name;
	const char *base_url;
	const char *model;
};

ProviderPreset const provider_presets[] = {
	{ "DeepSeek", "https://api.deepseek.com", "deepseek-v4-pro" },
	{ "OpenRouter", "https://openrouter.ai/api/v1", "deepseek/deepseek-chat-v3.1" },
	{ "Gemini OpenAI-compatible", "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-3-flash-preview" },
	{ "Alibaba Cloud Bailian", "https://dashscope.aliyuncs.com/compatible-mode/v1", "qwen3.5-plus" },
	{ "OpenAI-compatible custom", "https://api.openai.com/v1", "gpt-5.2" }
};

std::string json_escape_ai(std::string const& text) {
	std::string out;
	out.reserve(text.size() + 16);
	for (char c : text) {
		switch (c) {
			case '\\': out += "\\\\"; break;
			case '"': out += "\\\""; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20) {
					char buf[7];
					std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned char>(c));
					out += buf;
				}
				else {
					out += c;
				}
		}
	}
	return out;
}

std::string json_unescape_ai(std::string const& text) {
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] != '\\' || i + 1 >= text.size()) {
			out += text[i];
			continue;
		}
		char c = text[++i];
		switch (c) {
			case 'n': out += '\n'; break;
			case 'r': out += '\r'; break;
			case 't': out += '\t'; break;
			case '"': out += '"'; break;
			case '\\': out += '\\'; break;
			default: out += c; break;
		}
	}
	return out;
}

std::string extract_message_content(std::string const& json) {
	size_t key = json.find("\"content\"");
	if (key == std::string::npos)
		return {};
	size_t colon = json.find(':', key);
	if (colon == std::string::npos)
		return {};
	size_t quote = json.find('"', colon + 1);
	if (quote == std::string::npos)
		return {};

	std::string raw;
	bool escaped = false;
	for (size_t i = quote + 1; i < json.size(); ++i) {
		char c = json[i];
		if (escaped) {
			raw += '\\';
			raw += c;
			escaped = false;
			continue;
		}
		if (c == '\\') {
			escaped = true;
			continue;
		}
		if (c == '"')
			return json_unescape_ai(raw);
		raw += c;
	}
	return {};
}

std::string default_target_language() {
	std::string lang = OPT_GET("App/Language")->GetString();
	std::transform(lang.begin(), lang.end(), lang.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (lang.find("zh") == 0)
		return "Chinese";
	if (lang.find("ja") == 0)
		return "Japanese";
	if (lang.find("ko") == 0)
		return "Korean";
	if (lang.find("fr") == 0)
		return "French";
	if (lang.find("de") == 0)
		return "German";
	if (lang.find("es") == 0)
		return "Spanish";
	return "English";
}

std::string chat_completions_url(std::string base_url) {
	while (!base_url.empty() && base_url.back() == '/')
		base_url.pop_back();
	if (base_url.size() >= 17 && base_url.substr(base_url.size() - 17) == "/chat/completions")
		return base_url;
	return base_url + "/chat/completions";
}

#ifdef WITH_UPDATE_CHECKER
size_t write_response(char *ptr, size_t size, size_t nmemb, void *userdata) {
	auto out = static_cast<std::string *>(userdata);
	out->append(ptr, size * nmemb);
	return size * nmemb;
}

bool call_ai(std::string const& body, std::string const& base_url, std::string const& api_key, std::string& response, std::string& error) {
	CURL *curl = curl_easy_init();
	if (!curl) {
		error = "Failed to initialize libcurl.";
		return false;
	}

	curl_slist *headers = nullptr;
	std::string auth = "Authorization: Bearer " + api_key;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, auth.c_str());
	headers = curl_slist_append(headers, "Accept: application/json");

	std::string url = chat_completions_url(base_url);
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

	CURLcode code = curl_easy_perform(curl);
	long http_code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (code != CURLE_OK) {
		error = curl_easy_strerror(code);
		return false;
	}
	if (http_code < 200 || http_code >= 300) {
		error = "HTTP " + std::to_string(http_code) + ": " + response.substr(0, 1000);
		return false;
	}
	return true;
}
#endif

class DialogAIAnalysisSettings final : public wxDialog {
	wxCheckBox *enabled;
	wxChoice *provider;
	wxTextCtrl *base_url;
	wxTextCtrl *model;
	wxTextCtrl *api_key;
	wxTextCtrl *target_language;
	wxTextCtrl *temperature;
	wxSpinCtrl *max_tokens;
	wxCheckBox *thinking;
	wxCheckBox *cache;

	void OnProvider(wxCommandEvent&) {
		int sel = provider->GetSelection();
		if (sel >= 0 && sel < static_cast<int>(sizeof(provider_presets) / sizeof(provider_presets[0]))) {
			base_url->ChangeValue(provider_presets[sel].base_url);
			model->ChangeValue(provider_presets[sel].model);
		}
	}

	void OnOK(wxCommandEvent&) {
		if (thinking->GetValue()) {
			wxMessageBox(_("Only enable thinking when you know the selected model supports reasoning parameters. Unsupported models may reject the request."), _("AI Grammar Analysis Settings"), wxICON_INFORMATION);
		}
		session_api_key = from_wx(api_key->GetValue());
		OPT_SET("Tool/AI Analysis/Enabled")->SetBool(enabled->GetValue());
		OPT_SET("Tool/AI Analysis/Provider")->SetInt(provider->GetSelection());
		OPT_SET("Tool/AI Analysis/Base URL")->SetString(from_wx(base_url->GetValue()));
		OPT_SET("Tool/AI Analysis/Model")->SetString(from_wx(model->GetValue()));
		OPT_SET("Tool/AI Analysis/Target Language")->SetString(from_wx(target_language->GetValue()));
		OPT_SET("Tool/AI Analysis/Temperature")->SetDouble(std::max(0.0, std::min(2.0, wxAtof(temperature->GetValue()))));
		OPT_SET("Tool/AI Analysis/Max Tokens")->SetInt(max_tokens->GetValue());
		OPT_SET("Tool/AI Analysis/Thinking")->SetBool(thinking->GetValue());
		OPT_SET("Tool/AI Analysis/Cache")->SetBool(cache->GetValue());
		EndModal(wxID_OK);
	}

public:
	DialogAIAnalysisSettings(wxWindow *parent)
	: wxDialog(parent, -1, _("AI Grammar Analysis Settings"))
	{
		enabled = new wxCheckBox(this, -1, _("Enable AI button beside the subtitle text box"));
		enabled->SetValue(OPT_GET("Tool/AI Analysis/Enabled")->GetBool());

		wxArrayString providers;
		for (auto const& preset : provider_presets)
			providers.push_back(preset.name);
		provider = new wxChoice(this, -1, wxDefaultPosition, wxDefaultSize, providers);
		int provider_selection = static_cast<int>(OPT_GET("Tool/AI Analysis/Provider")->GetInt());
		provider->SetSelection(std::max(0, std::min(provider_selection, static_cast<int>(providers.size()) - 1)));

		base_url = new wxTextCtrl(this, -1, to_wx(OPT_GET("Tool/AI Analysis/Base URL")->GetString()));
		model = new wxTextCtrl(this, -1, to_wx(OPT_GET("Tool/AI Analysis/Model")->GetString()));
		api_key = new wxTextCtrl(this, -1, to_wx(session_api_key), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
		target_language = new wxTextCtrl(this, -1, to_wx(OPT_GET("Tool/AI Analysis/Target Language")->GetString().empty() ? default_target_language() : OPT_GET("Tool/AI Analysis/Target Language")->GetString()));
		temperature = new wxTextCtrl(this, -1, wxString::Format("%.2f", OPT_GET("Tool/AI Analysis/Temperature")->GetDouble()));
		max_tokens = new wxSpinCtrl(this, -1, "", wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 128, 32000, OPT_GET("Tool/AI Analysis/Max Tokens")->GetInt());
		thinking = new wxCheckBox(this, -1, _("Enable thinking/reasoning parameters"));
		thinking->SetValue(OPT_GET("Tool/AI Analysis/Thinking")->GetBool());
		cache = new wxCheckBox(this, -1, _("Cache identical analysis results in this session"));
		cache->SetValue(OPT_GET("Tool/AI Analysis/Cache")->GetBool());

		auto grid = new wxFlexGridSizer(2, 8, 6);
		grid->AddGrowableCol(1, 1);
		grid->Add(new wxStaticText(this, -1, _("Provider preset")), wxSizerFlags().Center().Right());
		grid->Add(provider, wxSizerFlags(1).Expand());
		grid->Add(new wxStaticText(this, -1, _("Base URL")), wxSizerFlags().Center().Right());
		grid->Add(base_url, wxSizerFlags(1).Expand());
		grid->Add(new wxStaticText(this, -1, _("Model")), wxSizerFlags().Center().Right());
		grid->Add(model, wxSizerFlags(1).Expand());
		grid->Add(new wxStaticText(this, -1, _("API key (kept in memory only)")), wxSizerFlags().Center().Right());
		grid->Add(api_key, wxSizerFlags(1).Expand());
		grid->Add(new wxStaticText(this, -1, _("Default target language")), wxSizerFlags().Center().Right());
		grid->Add(target_language, wxSizerFlags(1).Expand());
		grid->Add(new wxStaticText(this, -1, _("Temperature")), wxSizerFlags().Center().Right());
		grid->Add(temperature, wxSizerFlags(1).Expand());
		grid->Add(new wxStaticText(this, -1, _("Max tokens")), wxSizerFlags().Center().Right());
		grid->Add(max_tokens, wxSizerFlags(1).Expand());

		auto main = new wxBoxSizer(wxVERTICAL);
		main->Add(enabled, wxSizerFlags().Expand().Border());
		main->Add(grid, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(thinking, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(cache, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(CreateButtonSizer(wxOK | wxCANCEL), wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		SetSizerAndFit(main);
		SetMinSize(wxSize(620, -1));
		CenterOnParent();

		provider->Bind(wxEVT_CHOICE, &DialogAIAnalysisSettings::OnProvider, this);
		Bind(wxEVT_BUTTON, &DialogAIAnalysisSettings::OnOK, this, wxID_OK);
	}
};

class DialogAIAnalysis final : public wxDialog {
	std::string text;
	wxTextCtrl *target_language;
	wxTextCtrl *result;

	void Analyze(wxCommandEvent&) {
		if (!OPT_GET("Tool/AI Analysis/Enabled")->GetBool()) {
			DialogAIAnalysisSettings(this).ShowModal();
			if (!OPT_GET("Tool/AI Analysis/Enabled")->GetBool())
				return;
		}
		if (session_api_key.empty()) {
			DialogAIAnalysisSettings(this).ShowModal();
			if (session_api_key.empty())
				return;
		}

		std::string base_url = OPT_GET("Tool/AI Analysis/Base URL")->GetString();
		std::string model = OPT_GET("Tool/AI Analysis/Model")->GetString();
		std::string target = from_wx(target_language->GetValue());
		double temp = OPT_GET("Tool/AI Analysis/Temperature")->GetDouble();
		int max_tokens = OPT_GET("Tool/AI Analysis/Max Tokens")->GetInt();
		bool thinking = OPT_GET("Tool/AI Analysis/Thinking")->GetBool();

		std::string cache_key = base_url + "\n" + model + "\n" + target + "\n" + text + "\n" + (thinking ? "1" : "0");
		if (OPT_GET("Tool/AI Analysis/Cache")->GetBool()) {
			auto it = response_cache.find(cache_key);
			if (it != response_cache.end()) {
				result->SetValue(to_wx(it->second));
				return;
			}
		}

		std::string system = "You are a professional subtitle grammar and translation assistant. Analyze only the provided subtitle line. Ignore ASS override tags if any are present. Return concise sections: Text analysis, Grammar analysis, Recommended translation.";
		std::string user = "Target language: " + target + "\nSubtitle text:\n" + text;
		std::ostringstream body;
		body << "{\"model\":\"" << json_escape_ai(model) << "\",\"messages\":["
			<< "{\"role\":\"system\",\"content\":\"" << json_escape_ai(system) << "\"},"
			<< "{\"role\":\"user\",\"content\":\"" << json_escape_ai(user) << "\"}],"
			<< "\"temperature\":" << temp << ",\"max_tokens\":" << max_tokens;
		if (thinking)
			body << ",\"reasoning_effort\":\"low\"";
		if (thinking && base_url.find("deepseek") != std::string::npos)
			body << ",\"thinking\":{\"type\":\"enabled\"}";
		body << "}";

#ifdef WITH_UPDATE_CHECKER
		result->SetValue(_("Analyzing..."));
		wxSafeYield(this);
		std::string raw_response;
		std::string error;
		if (!call_ai(body.str(), base_url, session_api_key, raw_response, error)) {
			result->SetValue(to_wx(error));
			return;
		}
		std::string content = extract_message_content(raw_response);
		if (content.empty())
			content = raw_response;
		response_cache[cache_key] = content;
		result->SetValue(to_wx(content));
#else
		result->SetValue(_("This build was compiled without libcurl/update-checker support, so AI requests are unavailable."));
#endif
	}

	void CopyAll(wxCommandEvent&) {
		if (wxTheClipboard->Open()) {
			wxTheClipboard->SetData(new wxTextDataObject(result->GetValue()));
			wxTheClipboard->Close();
		}
	}

public:
	DialogAIAnalysis(wxWindow *parent, std::string line_text)
	: wxDialog(parent, -1, _("AI Grammar Analysis"), wxDefaultPosition, wxSize(720, 560))
	, text(std::move(line_text))
	{
		wxString target = to_wx(OPT_GET("Tool/AI Analysis/Target Language")->GetString().empty() ? default_target_language() : OPT_GET("Tool/AI Analysis/Target Language")->GetString());
		target_language = new wxTextCtrl(this, -1, target);
		auto source = new wxTextCtrl(this, -1, to_wx(text), wxDefaultPosition, wxSize(-1, 80), wxTE_MULTILINE | wxTE_READONLY);
		result = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);

		auto lang_sizer = new wxBoxSizer(wxHORIZONTAL);
		lang_sizer->Add(new wxStaticText(this, -1, _("Target language")), wxSizerFlags().Center().Border(wxRIGHT));
		lang_sizer->Add(target_language, wxSizerFlags(1).Expand());

		auto buttons = new wxBoxSizer(wxHORIZONTAL);
		auto analyze = new wxButton(this, -1, _("Analyze"));
		auto copy = new wxButton(this, -1, _("Copy All"));
		auto settings = new wxButton(this, -1, _("Settings"));
		buttons->Add(analyze, wxSizerFlags().Border(wxRIGHT));
		buttons->Add(copy, wxSizerFlags().Border(wxRIGHT));
		buttons->Add(settings, wxSizerFlags().Border(wxRIGHT));
		buttons->AddStretchSpacer();
		buttons->Add(new wxButton(this, wxID_CLOSE, _("Close")));

		auto main = new wxBoxSizer(wxVERTICAL);
		main->Add(lang_sizer, wxSizerFlags().Expand().Border());
		main->Add(source, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(result, wxSizerFlags(1).Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		main->Add(buttons, wxSizerFlags().Expand().Border(wxLEFT | wxRIGHT | wxBOTTOM));
		SetSizer(main);
		CenterOnParent();

		analyze->Bind(wxEVT_BUTTON, &DialogAIAnalysis::Analyze, this);
		copy->Bind(wxEVT_BUTTON, &DialogAIAnalysis::CopyAll, this);
		settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { DialogAIAnalysisSettings(this).ShowModal(); });
		Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
	}
};
}

void ShowAIAnalysisSettingsDialog(wxWindow *parent) {
	DialogAIAnalysisSettings(parent).ShowModal();
}

void ShowAIAnalysisDialog(agi::Context *c, std::string const& text) {
	std::string plain = text;
	if (plain.empty()) {
		if (auto line = c->selectionController->GetActiveLine())
			plain = line->GetStrippedText();
	}
	else {
		AssDialogue line;
		line.Text = boost::flyweight<std::string>(plain);
		plain = line.GetStrippedText();
	}

	DialogAIAnalysis(c->parent, plain).ShowModal();
}
