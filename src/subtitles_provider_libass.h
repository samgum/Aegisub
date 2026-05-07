// Copyright (c) 2014, Thomas Goyne <plorkyeran@aegisub.org>
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//
// Aegisub Project http://www.aegisub.org/

#pragma once

#include <cstddef>
#include <memory>
#include <string>

class SubtitlesProvider;
namespace agi { class BackgroundRunner; }

namespace libass {
	struct RenderedBounds {
		bool valid = false;
		int min_x = 0;
		int min_y = 0;
		int max_x = 0;
		int max_y = 0;
	};

	std::unique_ptr<SubtitlesProvider> Create(std::string const&, agi::BackgroundRunner *br);
	void CacheFonts();
	RenderedBounds GetRenderedBounds(const char *data, size_t len, int width, int height, int time_ms);
}
