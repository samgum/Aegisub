#pragma once

#include <vector>

class AssDialogue;
class wxDC;

namespace agi { struct Context; }

namespace subtitle_overflow {

struct Range {
	int start = 0;
	int length = 0;
};

struct Result {
	bool valid = false;
	bool overflow = false;
	std::vector<Range> ranges;
};

Result Check(agi::Context *context, AssDialogue const *line, wxDC *dc = nullptr);

void InvalidateLine(int id);
void InvalidateAll();

}
