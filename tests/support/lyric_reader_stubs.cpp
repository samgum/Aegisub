// Isolate the actual readers from application UI and style-catalog setup.
// Keep the real AssFile intrusive event list, AssDialogue fields, agi::Time,
// options and file I/O; the tests inspect exactly what ReadFile inserts.
#include "../../src/ass_attachment.h"
#include "../../src/ass_dialogue.h"
#include "../../src/ass_info.h"
#include "../../src/ass_style.h"
#include "../../src/ass_file.h"
#include "../../src/options.h"
#include "../../src/subtitle_format.h"

agi::Options *config::opt = nullptr;

AssFile::AssFile() = default;
AssFile::~AssFile() {
	Events.clear_and_dispose([](AssDialogue *line) { delete line; });
}
void AssFile::LoadDefault(bool, std::string const&) { }
AssDialogue::AssDialogue() { Id = 0; }
AssDialogue::~AssDialogue() = default;

SubtitleFormat::SubtitleFormat(std::string_view name) : name(name) { }
bool SubtitleFormat::CanReadFile(agi::fs::path const&, const char*) const { return false; }
bool SubtitleFormat::CanWriteFile(agi::fs::path const&) const { return false; }
bool SubtitleFormat::CanSave(const AssFile*) const { return false; }

AssEntryGroup AssAttachment::Group() const { return AssEntryGroup::FONT; }
