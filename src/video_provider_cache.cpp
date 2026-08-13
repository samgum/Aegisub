// Copyright (c) 2013, Thomas Goyne <plorkyeran@aegisub.org>
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

#include "include/aegisub/video_provider.h"

#include "options.h"
#include "video_frame.h"


#include <iterator>
#include <list>
#include <unordered_map>

namespace {
/// A video frame and its frame number
struct CachedFrame {
	VideoFrame frame;
	int frame_number;

	CachedFrame(VideoFrame const& frame, int frame_number)
	: frame(frame), frame_number(frame_number) { }

	CachedFrame(CachedFrame const&) = delete;
};

/// @class VideoProviderCache
/// @brief A wrapper around a video provider which provides LRU caching
class VideoProviderCache final : public VideoProvider {
	using Cache = std::list<CachedFrame>;
	/// The source provider to get frames from
	std::unique_ptr<VideoProvider> master;

	/// @brief Maximum size of the cache in bytes
	///
	/// Note that this is a soft limit. The cache stops allocating new frames
	/// once it has exceeded the limit, but it never tries to shrink
	const size_t max_cache_size = OPT_GET("Provider/Video/Cache/Size")->GetInt() << 20; // convert MB to bytes

	/// Cache of video frames with the most recently used ones at the front
	Cache cache;
	std::unordered_map<int, Cache::iterator> index;
	size_t total_size = 0;

public:
	VideoProviderCache(std::unique_ptr<VideoProvider> master) : master(std::move(master)) { }

	void GetFrame(int n, VideoFrame &frame) override;

	void SetColorSpace(std::string const& m) override {
		cache.clear();
		index.clear();
		total_size = 0;
		return master->SetColorSpace(m);
	}

	int GetFrameCount() const override             { return master->GetFrameCount(); }
	int GetWidth() const override                  { return master->GetWidth(); }
	int GetHeight() const override                 { return master->GetHeight(); }
	double GetDAR() const override                 { return master->GetDAR(); }
	agi::vfr::Framerate GetFPS() const override    { return master->GetFPS(); }
	std::vector<int> GetKeyFrames() const override { return master->GetKeyFrames(); }
	std::string GetWarning() const override        { return master->GetWarning(); }
	std::string GetDecoderName() const override    { return master->GetDecoderName(); }
	std::string GetColorSpace() const override     { return master->GetColorSpace(); }
	std::string GetRealColorSpace() const override { return master->GetRealColorSpace(); }
	bool ShouldSetVideoProperties() const override { return master->ShouldSetVideoProperties(); }
	bool HasAudio() const override                 { return master->HasAudio(); }
};

void VideoProviderCache::GetFrame(int n, VideoFrame &out) {
	auto hit = index.find(n);
	if (hit != index.end()) {
		cache.splice(cache.begin(), cache, hit->second); // Move to front
		hit->second = cache.begin();
		out = hit->second->frame;
		return;
	}

	master->GetFrame(n, out);

	// A zero cache budget means caching is disabled. Without this guard the
	// empty-list safety path below retained one full frame indefinitely.
	if (max_cache_size == 0)
		return;

	// Keep the exact byte budget when possible. A single oversized frame is
	// still cached (the documented soft-limit behavior), but never alongside
	// stale entries. This also makes duplicate frame numbers impossible.
	auto frame_size = out.data.size();
	while (!cache.empty() && (total_size > max_cache_size || frame_size > max_cache_size - total_size)) {
		auto victim = std::prev(cache.end());
		total_size -= victim->frame.data.size();
		index.erase(victim->frame_number);
		cache.erase(victim);
	}

	cache.emplace_front(out, n);
	index.emplace(n, cache.begin());
	total_size += frame_size;
}
}

std::unique_ptr<VideoProvider> CreateCacheVideoProvider(std::unique_ptr<VideoProvider> parent) {
	return std::make_unique<VideoProviderCache>(std::move(parent));
}
