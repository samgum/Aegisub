// Copyright (c) 2006-2007, Rodrigo Braz Monteiro, Evgeniy Stepanov
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

/// @file subtitles_provider_libass.cpp
/// @brief libass-based subtitle renderer
/// @ingroup subtitle_rendering
///

#include "subtitles_provider_libass.h"

#include "ass_attachment.h"
#include "ass_dialogue.h"
#include "ass_file.h"
#include "ass_info.h"
#include "ass_style.h"
#include "compat.h"
#include "include/aegisub/subtitles_provider.h"
#include "video_frame.h"

#include <libaegisub/background_runner.h>
#include <libaegisub/dispatch.h>
#include <libaegisub/exception.h>
#include <libaegisub/log.h>
#include <libaegisub/util.h>

#include <algorithm>
#include <atomic>
#include <boost/gil.hpp>
#include <climits>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <wx/intl.h>
#include <wx/thread.h>

extern "C" {
#include <ass/ass.h>
}

namespace {
std::unique_ptr<agi::dispatch::Queue> cache_queue;
ASS_Library *library;
std::mutex overflow_renderer_mutex;
ASS_Renderer *overflow_renderer = nullptr;

void msg_callback(int level, const char *fmt, va_list args, void *) {
	if (level >= 7) return;
	char buf[1024];
#ifdef _WIN32
	vsprintf_s(buf, sizeof(buf), fmt, args);
#else
	vsnprintf(buf, sizeof(buf), fmt, args);
#endif

	if (level < 2) // warning/error
		LOG_I("subtitle/provider/libass") << buf;
	else // verbose
		LOG_D("subtitle/provider/libass") << buf;
}

std::vector<char> serialize_subtitles(AssFile *subs) {
	std::vector<char> buffer;
	auto push_header = [&](const char *str) {
		buffer.insert(buffer.end(), str, str + strlen(str));
	};
	auto push_line = [&](std::string const& str) {
		buffer.insert(buffer.end(), &str[0], &str[0] + str.size());
		buffer.push_back('\n');
	};

	push_header("\xEF\xBB\xBF[Script Info]\n");
	for (auto const& line : subs->Info)
		push_line(line.GetEntryData());

	push_header("[V4+ Styles]\n");
	for (auto const& line : subs->Styles)
		push_line(line.GetEntryData());

	if (!subs->Attachments.empty()) {
		push_header("[Fonts]\n");
		for (auto const& attachment : subs->Attachments)
			if (attachment.Group() == AssEntryGroup::FONT)
				push_line(attachment.GetEntryData());
	}

	push_header("[Events]\n");
	for (auto const& line : subs->Events) {
		if (!line.Comment)
			push_line(line.GetEntryData());
	}

	return buffer;
}

ASS_Renderer *get_overflow_renderer() {
	if (!overflow_renderer) {
		overflow_renderer = ass_renderer_init(library);
		ass_set_font_scale(overflow_renderer, 1.);
		ass_set_fonts(overflow_renderer, nullptr, "Sans", 1, nullptr, true);
	}
	return overflow_renderer;
}

int count_bands(std::vector<std::pair<int, int>>& bands) {
	if (bands.empty())
		return 0;

	std::sort(bands.begin(), bands.end());
	int count = 0;
	int current_bottom = INT_MIN;
	for (auto const& band : bands) {
		if (band.first > current_bottom + 3)
			++count;
		current_bottom = std::max(current_bottom, band.second);
	}
	return count;
}

// Stuff used on the cache thread, owned by a shared_ptr in case the provider
// gets deleted before the cache finishing updating
struct cache_thread_shared {
	ASS_Renderer *renderer = nullptr;
	std::atomic<bool> ready{false};
	~cache_thread_shared() { if (renderer) ass_renderer_done(renderer); }
};

class LibassSubtitlesProvider final : public SubtitlesProvider {
	agi::BackgroundRunner *br;
	std::shared_ptr<cache_thread_shared> shared;
	ASS_Track* ass_track = nullptr;

	ASS_Renderer *renderer() {
		if (shared->ready)
			return shared->renderer;

		auto block = [&] {
			if (shared->ready)
				return;
			agi::util::sleep_for(250);
			if (shared->ready)
				return;
			br->Run([this](agi::ProgressSink *ps) {
				ps->SetTitle(from_wx(_("Updating font index")));
				ps->SetMessage(from_wx(_("This may take several minutes")));
				ps->SetIndeterminate();
				while (!shared->ready && !ps->IsCancelled())
					agi::util::sleep_for(250);
			});
		};

		if (wxThread::IsMain())
			block();
		else
			agi::dispatch::Main().Sync(block);
		return shared->renderer;
	}

public:
	LibassSubtitlesProvider(agi::BackgroundRunner *br);
	~LibassSubtitlesProvider();

	void LoadSubtitles(const char *data, size_t len) override {
		if (ass_track) ass_free_track(ass_track);
		ass_track = ass_read_memory(library, const_cast<char *>(data), len, nullptr);
		if (!ass_track) throw agi::InternalError("libass failed to load subtitles.");
	}

	void DrawSubtitles(VideoFrame &dst, double time) override;

	void Reinitialize() override {
		// No need to reinit if we're not even done with the initial init
		if (!shared->ready)
			return;

		ass_renderer_done(shared->renderer);
		shared->renderer = ass_renderer_init(library);
		ass_set_font_scale(shared->renderer, 1.);
		ass_set_fonts(shared->renderer, nullptr, "Sans", 1, nullptr, true);
	}
};

LibassSubtitlesProvider::LibassSubtitlesProvider(agi::BackgroundRunner *br)
: br(br)
, shared(std::make_shared<cache_thread_shared>())
{
	auto state = shared;
	cache_queue->Async([state] {
		auto ass_renderer = ass_renderer_init(library);
		if (ass_renderer) {
			ass_set_font_scale(ass_renderer, 1.);
			ass_set_fonts(ass_renderer, nullptr, "Sans", 1, nullptr, true);
		}
		state->renderer = ass_renderer;
		state->ready = true;
	});
}

LibassSubtitlesProvider::~LibassSubtitlesProvider() {
	if (ass_track) ass_free_track(ass_track);
}

#define _r(c) ((c)>>24)
#define _g(c) (((c)>>16)&0xFF)
#define _b(c) (((c)>>8)&0xFF)
#define _a(c) ((c)&0xFF)

void LibassSubtitlesProvider::DrawSubtitles(VideoFrame &frame,double time) {
	ass_set_frame_size(renderer(), frame.width, frame.height);
	// Note: this relies on Aegisub always rendering at video storage res
	ass_set_storage_size(renderer(), frame.width, frame.height);

	ASS_Image* img = ass_render_frame(renderer(), ass_track, int(time * 1000), nullptr);

	// libass actually returns several alpha-masked monochrome images.
	// Here, we loop through their linked list, get the colour of the current, and blend into the frame.
	// This is repeated for all of them.

	using namespace boost::gil;
	auto dst = interleaved_view(frame.width, frame.height, (bgra8_pixel_t*)frame.data.data(), frame.width * 4);
	if (frame.flipped)
		dst = flipped_up_down_view(dst);

	for (; img; img = img->next) {
		unsigned int opacity = 255 - ((unsigned int)_a(img->color));
		unsigned int r = (unsigned int)_r(img->color);
		unsigned int g = (unsigned int)_g(img->color);
		unsigned int b = (unsigned int)_b(img->color);

		auto srcview = interleaved_view(img->w, img->h, (gray8_pixel_t*)img->bitmap, img->stride);
		auto dstview = subimage_view(dst, img->dst_x, img->dst_y, img->w, img->h);

		transform_pixels(dstview, srcview, dstview, [=](const bgra8_pixel_t frame, const gray8_pixel_t src) -> bgra8_pixel_t {
			unsigned int k = ((unsigned)src) * opacity / 255;
			unsigned int ck = 255 - k;

			bgra8_pixel_t ret;
			ret[0] = (k * b + ck * frame[0]) / 255;
			ret[1] = (k * g + ck * frame[1]) / 255;
			ret[2] = (k * r + ck * frame[2]) / 255;
			ret[3] = 0;
			return ret;
		});
	}
}
}

namespace libass {
std::unique_ptr<SubtitlesProvider> Create(std::string const&, agi::BackgroundRunner *br) {
	return std::make_unique<LibassSubtitlesProvider>(br);
}

RenderedBounds GetRenderedBounds(AssFile *subs, int time_ms, int width, int height) {
	RenderedBounds bounds;
	if (!subs || !library || width <= 0 || height <= 0)
		return bounds;

	std::lock_guard<std::mutex> lock(overflow_renderer_mutex);
	auto renderer = get_overflow_renderer();
	if (!renderer)
		return bounds;

	auto data = serialize_subtitles(subs);
	ASS_Track *track = ass_read_memory(library, data.data(), data.size(), nullptr);
	if (!track)
		return bounds;

	ass_set_frame_size(renderer, width, height);
	ass_set_storage_size(renderer, width, height);

	ASS_Image *img = ass_render_frame(renderer, track, time_ms, nullptr);
	bounds.valid = true;
	std::vector<std::pair<int, int>> y_bands;

	for (; img; img = img->next) {
		unsigned int opacity = 255 - ((unsigned int)_a(img->color));
		if (!opacity || img->w <= 0 || img->h <= 0)
			continue;

		int img_left = INT_MAX;
		int img_top = INT_MAX;
		int img_right = INT_MIN;
		int img_bottom = INT_MIN;

		for (int y = 0; y < img->h; ++y) {
			auto row = img->bitmap + y * img->stride;
			int row_left = -1;
			int row_right = -1;
			for (int x = 0; x < img->w; ++x) {
				if (!row[x])
					continue;
				if (row_left < 0)
					row_left = x;
				row_right = x + 1;
			}

			if (row_left < 0)
				continue;

			img_left = std::min(img_left, img->dst_x + row_left);
			img_right = std::max(img_right, img->dst_x + row_right);
			img_top = std::min(img_top, img->dst_y + y);
			img_bottom = std::max(img_bottom, img->dst_y + y + 1);
		}

		if (img_left == INT_MAX)
			continue;

		if (!bounds.has_pixels) {
			bounds.left = img_left;
			bounds.top = img_top;
			bounds.right = img_right;
			bounds.bottom = img_bottom;
			bounds.has_pixels = true;
		}
		else {
			bounds.left = std::min(bounds.left, img_left);
			bounds.top = std::min(bounds.top, img_top);
			bounds.right = std::max(bounds.right, img_right);
			bounds.bottom = std::max(bounds.bottom, img_bottom);
		}
		y_bands.emplace_back(img_top, img_bottom);
	}

	bounds.bands = count_bands(y_bands);
	ass_free_track(track);
	return bounds;
}

void CacheFonts() {
	// Initialize the cache worker thread
	cache_queue = agi::dispatch::Create();

	// Initialize libass
	library = ass_library_init();
	ass_set_message_cb(library, msg_callback, nullptr);

	// Initialize a renderer to force fontconfig to update its cache
	cache_queue->Async([] {
		auto ass_renderer = ass_renderer_init(library);
		ass_set_fonts(ass_renderer, nullptr, "Sans", 1, nullptr, true);
		ass_renderer_done(ass_renderer);
	});
}
}
