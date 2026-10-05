#include "../../src/update_checker.h"

#include <libaegisub/cajun/reader.h>
#include <gtest/gtest.h>

#include <sstream>

namespace {

json::UnknownElement Read(std::string const& text) {
	std::istringstream stream(text);
	json::UnknownElement value;
	json::Reader::Read(value, stream);
	return value;
}

json::UnknownElement Release() {
	return Read(R"({
		"tag_name": "Aegisub-SGMY-2026-10-05",
		"html_url": "https://github.com/samgum/Aegisub/releases/tag/Aegisub-SGMY-2026-10-05",
		"name": "Aegisub SGMY 2026-10-05",
		"body": "中文更新日志\n\n---\n\nEnglish release notes",
		"draft": false, "prerelease": false
	})");
}

TEST(UpdateChecker, DisplaysPersonalReleaseAndBilingualNotes) {
	auto release = sgmy::updates::ParseRelease(Release());
	ASSERT_TRUE(release);
	EXPECT_EQ("Aegisub-SGMY-2026-10-05", release->tag);
	EXPECT_EQ("Aegisub SGMY 2026-10-05", release->name);
	EXPECT_EQ("https://github.com/samgum/Aegisub/releases/tag/Aegisub-SGMY-2026-10-05", release->url);
	EXPECT_EQ("中文更新日志\n\n---\n\nEnglish release notes", release->description);
}

TEST(UpdateChecker, IgnoresDraftsAndPrereleases) {
	for (char const* field : {"draft", "prerelease"}) {
		auto value = Release();
		static_cast<json::Object&>(value)[field] = true;
		EXPECT_FALSE(sgmy::updates::ParseRelease(value));
	}
}

TEST(UpdateChecker, IgnoresUpstreamTags) {
	auto value = Release();
	static_cast<json::Object&>(value)["tag_name"] = "v3.5.0";
	EXPECT_FALSE(sgmy::updates::ParseRelease(value));
}

TEST(UpdateChecker, AllowsUnnamedReleaseWithoutNotes) {
	auto value = Release();
	auto& object = static_cast<json::Object&>(value);
	object["name"] = json::Null{};
	object["body"] = json::Null{};
	auto release = sgmy::updates::ParseRelease(value);
	ASSERT_TRUE(release);
	EXPECT_EQ(release->tag, release->name);
	EXPECT_TRUE(release->description.empty());
}

TEST(UpdateChecker, RejectsUnexpectedDownloadDestination) {
	auto value = Release();
	static_cast<json::Object&>(value)["html_url"] = "https://aegisub.org/downloads/";
	EXPECT_THROW(sgmy::updates::ParseRelease(value), std::runtime_error);
}

TEST(UpdateChecker, RejectsUnsafeOrEmptyTags) {
	for (char const* tag : {"Aegisub-SGMY-", "Aegisub-SGMY-2026/10/05", "Aegisub-SGMY-2026-10-05?x=y"}) {
		auto value = Release();
		static_cast<json::Object&>(value)["tag_name"] = tag;
		EXPECT_THROW(sgmy::updates::ParseRelease(value), std::runtime_error);
	}
}

TEST(UpdateChecker, RejectsMalformedResponsesInsteadOfClaimingNoUpdates) {
	EXPECT_THROW(sgmy::updates::ParseRelease(Read(R"({"message":"API rate limit exceeded"})")), std::exception);
	auto value = Release();
	static_cast<json::Object&>(value)["draft"] = "false";
	EXPECT_THROW(sgmy::updates::ParseRelease(value), json::Exception);
}

TEST(UpdateChecker, OffersDescendantReleaseIncludingSameDayBuilds) {
	EXPECT_TRUE(sgmy::updates::IsNewerRelease(Read(R"({"status":"ahead","ahead_by":1})")));
}

TEST(UpdateChecker, DoesNotOfferSameCommitOrDowngradeDevelopmentBuild) {
	for (char const* status : {"identical", "behind", "diverged"}) {
		EXPECT_FALSE(sgmy::updates::IsNewerRelease(Read(std::string("{\"status\":\"") + status + "\",\"ahead_by\":0}")));
	}
	EXPECT_FALSE(sgmy::updates::IsNewerRelease(Read(R"({"status":"ahead","ahead_by":0})")));
}

TEST(UpdateChecker, RejectsInvalidComparison) {
	EXPECT_THROW(sgmy::updates::IsNewerRelease(Read(R"({"message":"Not Found"})")), std::exception);
	EXPECT_THROW(sgmy::updates::IsNewerRelease(Read(R"({"status":"unknown"})")), std::runtime_error);
	EXPECT_THROW(sgmy::updates::IsNewerRelease(Read(R"({"status":"ahead","ahead_by":"1"})")), json::Exception);
}

TEST(UpdateChecker, ValidatesFullBuildCommit) {
	EXPECT_TRUE(sgmy::updates::IsBuildCommit("5cdd1f0fbad1fc7943531f233097eb9fb9d569f1"));
	EXPECT_FALSE(sgmy::updates::IsBuildCommit("5cdd1f0"));
	EXPECT_FALSE(sgmy::updates::IsBuildCommit(""));
	EXPECT_FALSE(sgmy::updates::IsBuildCommit(std::string(40, 'z')));
	EXPECT_FALSE(sgmy::updates::IsBuildCommit("master"));
}

}
