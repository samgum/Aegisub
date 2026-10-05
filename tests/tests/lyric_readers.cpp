#include "../../src/ass_attachment.h"
#include "../../src/ass_dialogue.h"
#include "../../src/ass_info.h"
#include "../../src/ass_style.h"
#include "../../src/ass_file.h"
#include "../../src/options.h"
#include "../../src/subtitle_format_lrc.h"
#include "../../src/subtitle_format_ttml.h"

#include <libaegisub/vfr.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
class LyricReaders : public ::testing::Test {
	std::unique_ptr<agi::Options> options;
	std::filesystem::path directory;
protected:
	AssFile file;
	void SetUp() override {
		directory = std::filesystem::temp_directory_path() / ("aegisub-lyric-test-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(directory);
		options = std::make_unique<agi::Options>(agi::fs::path((directory / "options.json").string()),
			R"({"Subtitle Format":{"TXT":{"Default Style Catalog":""},"TTXT":{"Default Style Catalog":""}}})",
			agi::Options::FLUSH_SKIP);
		config::opt = options.get();
	}
	void TearDown() override {
		config::opt = nullptr;
		std::filesystem::remove_all(directory);
	}
	void Read(std::string const& text, bool ttml = false) {
		auto path = directory / (ttml ? "lyrics.ttml" : "lyrics.lrc");
		std::ofstream(path, std::ios::binary) << text;
		if (ttml) TTMLSubtitleFormat().ReadFile(&file, agi::fs::path(path.string()), agi::vfr::Framerate(), "utf-8");
		else LrcSubtitleFormat().ReadFile(&file, agi::fs::path(path.string()), agi::vfr::Framerate(), "utf-8");
	}
	std::string Text(size_t index = 0) {
		return std::next(file.Events.begin(), index)->Text.get();
	}
};

TEST_F(LyricReaders, LrcIntegerSecondsAndFractions) {
	Read("[00:12]first\n[00:13.5]second\n[00:14.25]third\n");
	ASSERT_EQ(std::distance(file.Events.begin(), file.Events.end()), 3);
	EXPECT_EQ(int(file.Events.front().Start), 12000);
	EXPECT_EQ(Text(), "first");
}
TEST_F(LyricReaders, LrcRejectsPartialAndOverflowingTimes) {
	Read("[00:12x]bad\n[00:12.2x]bad\n[9223372036854775807:00.00]bad\n[00:20.00]good\n");
	ASSERT_EQ(std::distance(file.Events.begin(), file.Events.end()), 1);
	EXPECT_EQ(Text(), "good");
}
TEST_F(LyricReaders, LrcShortRowsDoNotOverlapOrExtendExplicitEnd) {
	Read("[00:01.00]<00:01.00>a<00:01.10>\n[00:01.20]b\n");
	EXPECT_EQ(int(file.Events.front().End), 1100);
}
TEST_F(LyricReaders, LrcRepeatedWordTimingsAreShifted) {
	Read("[00:01.00][00:11.00]<00:01.00>a<00:02.00>b<00:03.00>\n");
	ASSERT_EQ(std::distance(file.Events.begin(), file.Events.end()), 2);
	EXPECT_EQ(Text(0), "{\\kf100}a{\\kf100}b");
	EXPECT_EQ(Text(1), Text(0));
	EXPECT_EQ(int(file.Events.back().End), 13000);
}
TEST_F(LyricReaders, LrcPreservesDelayBeforeFirstWord) {
	Read("[00:01.00]<00:02.00>a<00:03.00>\n");
	EXPECT_EQ(Text(), "{\\k100}{\\kf100}a");
}
TEST_F(LyricReaders, LrcOffsetAfterLyricsAppliesToWholeFile) {
	Read("[00:02.00]a\n[offset:+500]\n");
	EXPECT_EQ(int(file.Events.front().Start), 1500);
}
TEST_F(LyricReaders, TtmlNamespacePrefixesAndBreaks) {
	Read(R"(<t:tt xmlns:t="http://www.w3.org/ns/ttml"><t:body><t:div><t:p begin="1s" end="2s">a<t:br/>b</t:p></t:div></t:body></t:tt>)", true);
	EXPECT_EQ(Text(), "a\\Nb");
}
TEST_F(LyricReaders, TtmlUntimedRowsKeepDocumentOrder) {
	Read("<tt><body><div><p>first</p><p>second</p><p>third</p></div></body></tt>", true);
	ASSERT_EQ(std::distance(file.Events.begin(), file.Events.end()), 3);
	EXPECT_EQ(Text(0), "first");
	EXPECT_EQ(Text(2), "third");
}
TEST_F(LyricReaders, TtmlUntimedWrappersDoNotDuplicateOrReorderText) {
	Read("<tt><body><p begin=\"1s\" end=\"2s\">a<span>b</span><span begin=\"1s\" end=\"2s\">c</span><span>d</span></p></body></tt>", true);
	EXPECT_EQ(Text(), "ab{\\kf100}cd");
}
TEST_F(LyricReaders, TtmlKaraokeKeepsInitialDelayAndWordGaps) {
	Read("<tt><body><p begin=\"1s\" end=\"5s\"><span begin=\"2s\" end=\"2.5s\">a</span><span begin=\"4s\" end=\"5s\">b</span></p></body></tt>", true);
	EXPECT_EQ(Text(), "{\\k100}{\\kf50}a{\\k150}{\\kf100}b");
}
TEST_F(LyricReaders, TtmlRejectsNonfiniteAndOverflowingTimes) {
	Read("<tt><body><p begin=\"1e300s\" end=\"2s\">bad</p><p begin=\"nan\" end=\"inf\">bad</p><p begin=\"1.5s\" end=\"2.5s\">good</p></body></tt>", true);
	ASSERT_EQ(std::distance(file.Events.begin(), file.Events.end()), 1);
	EXPECT_EQ(int(file.Events.front().Start), 1500);
}
TEST_F(LyricReaders, TtmlUnicodeTextIsUTF8) {
	Read("<tt><body><p begin=\"1s\" end=\"2s\">中文歌词・日本語</p></body></tt>", true);
	EXPECT_EQ(Text(), "中文歌词・日本語");
}
TEST_F(LyricReaders, TtmlPreambleKeepsWordSeparatingSpace) {
	Read("<tt><body><p begin=\"1s\" end=\"2s\">Hello <span begin=\"1s\" end=\"2s\">world</span></p></body></tt>", true);
	EXPECT_EQ(Text(), "Hello {\\kf100}world");
}
TEST_F(LyricReaders, TtmlBackgroundVoiceKeepsItsOwnTiming) {
	Read(R"(<tt><body><p begin="1s" end="5s"><span begin="1s" end="3s">a</span><span begin="4s" end="5s">b</span><span xmlns:ttm="http://www.w3.org/ns/ttml#metadata" ttm:role="x-bg"><span begin="2s" end="2.5s">x</span></span></p></body></tt>)", true);
	ASSERT_EQ(std::distance(file.Events.begin(), file.Events.end()), 2);
	EXPECT_EQ(Text(0), "{\\kf200}a{\\k100}{\\kf100}b");
	EXPECT_EQ(Text(1), "{\\kf50}x");
	EXPECT_EQ(int(file.Events.back().Start), 2000);
}
TEST_F(LyricReaders, TtmlClockAndOffsetDurations) {
	Read(R"(<tt><body><p begin="00:00:01.500" dur="500ms">a</p><p begin="00:02,50" end="3s">b</p></body></tt>)", true);
	ASSERT_EQ(std::distance(file.Events.begin(), file.Events.end()), 2);
	EXPECT_EQ(int(file.Events.front().Start), 1500);
	EXPECT_EQ(int(file.Events.front().End), 2000);
	EXPECT_EQ(int(file.Events.back().Start), 2500);
}
TEST_F(LyricReaders, LrcNearbyRowsKeepTheirShortDuration) {
	Read("[00:01.00]a\n[00:01.20]b\n");
	EXPECT_EQ(int(file.Events.front().End), 1200);
}
TEST_F(LyricReaders, UntimedLrcPreservesRows) {
	Read("first\nsecond\n");
	ASSERT_EQ(std::distance(file.Events.begin(), file.Events.end()), 2);
	EXPECT_EQ(Text(0), "first");
	EXPECT_EQ(Text(1), "second");
}

}
