#pragma once

#include <libaegisub/cajun/elements.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sgmy::updates {

struct Release {
	std::string tag;
	std::string url;
	std::string name;
	std::string description;
};

inline bool IsBuildCommit(std::string_view commit) {
	return commit.size() == 40 && std::all_of(commit.begin(), commit.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	});
}

inline std::string OptionalString(json::Object const& object, char const* key) {
	auto it = object.find(key);
	if (it == object.end()) return {};
	try {
		return static_cast<json::String const&>(it->second);
	}
	catch (json::Exception const&) {
		static_cast<json::Null const&>(it->second);
		return {};
	}
}

inline std::optional<Release> ParseRelease(json::UnknownElement const& value) {
	auto const& object = static_cast<json::Object const&>(value);
	if (static_cast<json::Boolean>(object.at("draft")) ||
	    static_cast<json::Boolean>(object.at("prerelease")))
		return std::nullopt;

	auto const& tag = static_cast<json::String const&>(object.at("tag_name"));
	constexpr std::string_view prefix = "Aegisub-SGMY-";
	if (!std::string_view(tag).starts_with(prefix)) return std::nullopt;
	if (tag.size() == prefix.size() || !std::all_of(tag.begin(), tag.end(), [](char c) {
		return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
		       (c >= 'a' && c <= 'z') || c == '-' || c == '.' || c == '_';
	}))
		throw std::runtime_error("Invalid SGMY release tag");

	std::string url = "https://github.com/samgum/Aegisub/releases/tag/" + tag;
	if (static_cast<json::String const&>(object.at("html_url")) != url)
		throw std::runtime_error("Unexpected SGMY release URL");
	std::string name = OptionalString(object, "name");
	if (name.empty()) name = tag;
	return Release{tag, std::move(url), std::move(name), OptionalString(object, "body")};
}

// GitHub compares base (this build) ... head (the release tag). A newer
// development build must not be offered an older release, even on the same day.
inline bool IsNewerRelease(json::UnknownElement const& comparison) {
	auto const& object = static_cast<json::Object const&>(comparison);
	auto const& status = static_cast<json::String const&>(object.at("status"));
	if (status == "ahead")
		return static_cast<json::Integer>(object.at("ahead_by")) > 0;
	if (status == "behind" || status == "identical" || status == "diverged")
		return false;
	throw std::runtime_error("Invalid GitHub comparison status");
}

}
