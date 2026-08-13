// Copyright (c) 2008-2009, Karl Blomster
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

/// @file video_provider_ffmpegsource.cpp
/// @brief FFmpegSource2-based video provider
/// @ingroup video_input ffms
///

#ifdef WITH_FFMS2
#include "ffmpegsource_common.h"
#include "hdr_tonemap.h"
#include "include/aegisub/video_provider.h"

#include "options.h"
#include "utils.h"
#include "video_frame.h"

#include <libaegisub/fs.h>
#include <libaegisub/log.h>

#include <cstdint>
#include <cstring>
#include <string_view>

namespace {
int normalize_rotation(int rotation) {
	rotation %= 360;
	return rotation < 0 ? rotation + 360 : rotation;
}

typedef enum AGI_ColorSpaces {
	AGI_CS_RGB = 0,
	AGI_CS_BT709 = 1,
	AGI_CS_UNSPECIFIED = 2,
	AGI_CS_FCC = 4,
	AGI_CS_BT470BG = 5,
	AGI_CS_SMPTE170M = 6,
	AGI_CS_SMPTE240M = 7,
	AGI_CS_YCOCG = 8,
	AGI_CS_BT2020_NCL = 9,
	AGI_CS_BT2020_CL = 10,
	AGI_CS_SMPTE2085 = 11,
	AGI_CS_CHROMATICITY_DERIVED_NCL = 12,
	AGI_CS_CHROMATICITY_DERIVED_CL = 13,
	AGI_CS_ICTCP = 14,
	AGI_CS_IPT_C2 = 15
} AGI_ColorSpaces;

bool swscale_has_input_matrix(int color_space) {
	switch (color_space) {
		case AGI_CS_RGB:
		case AGI_CS_BT709:
		case AGI_CS_FCC:
		case AGI_CS_BT470BG:
		case AGI_CS_SMPTE170M:
		case AGI_CS_SMPTE240M:
		case AGI_CS_BT2020_NCL:
		case AGI_CS_BT2020_CL:
			return true;
		default:
			return false;
	}
}

void copy_bgra(unsigned char *dst, unsigned char const *src) {
	uint32_t pixel;
	std::memcpy(&pixel, src, sizeof pixel);
	std::memcpy(dst, &pixel, sizeof pixel);
}

void swap_bgra(unsigned char *lhs, unsigned char *rhs) {
	uint32_t a, b;
	std::memcpy(&a, lhs, sizeof a);
	std::memcpy(&b, rhs, sizeof b);
	std::memcpy(lhs, &b, sizeof b);
	std::memcpy(rhs, &a, sizeof a);
}

/// @class FFmpegSourceVideoProvider
/// @brief Implements video loading through the FFMS library.
class FFmpegSourceVideoProvider final : public VideoProvider, FFmpegSourceProvider {
	/// video source object
	agi::scoped_holder<FFMS_VideoSource*, void (FFMS_CC*)(FFMS_VideoSource*)> VideoSource;
	const FFMS_VideoProperties *VideoInfo = nullptr; ///< video properties

	int Width = -1;                 ///< width in pixels
	int Height = -1;                ///< height in pixels
	int CS = -1;                    ///< Reported colorspace of first frame
	int CR = -1;                    ///< Reported colorrange of first frame
	// HDR metadata, captured from the first frame so the provider can decide
	// whether to tone-map. NOTE: FFMS2 spells TransferCharateristics without
	// the second 'c' in 'cteristics' — this matches the upstream ffms.h.
	int Transfer = -1;              ///< Reported transfer characteristics (AVColorTransferCharacteristic)
	int Primaries = -1;             ///< Reported color primaries (AVColorPrimaries)
	int MaxCLL = 0;                 ///< Max content light level (nits); 0 = unavailable
	bool HasDolbyVision = false;    ///< True when FFMS2 exposes Dolby Vision RPU data
	bool IsHDR = false;             ///< True for PQ/HLG or Dolby Vision sources
	bool UseCpuToneMap = false;     ///< Standard PQ/HLG base layer can use the preview mapper
	std::unique_ptr<HDRTonemap::ToneMapper> CpuToneMapper;
	double DAR;                     ///< display aspect ratio
	std::vector<int> KeyFramesList; ///< list of keyframes
	agi::vfr::Framerate Timecodes;  ///< vfr object
	std::string ColorSpace;         ///< Colorspace name
	std::string RealColorSpace;     ///< Colorspace name

	char FFMSErrMsg[1024];          ///< FFMS error message
	FFMS_ErrorInfo ErrInfo;         ///< FFMS error codes/messages
	bool has_audio = false;

	void LoadVideo(agi::fs::path const& filename, std::string_view colormatrix);

public:
	FFmpegSourceVideoProvider(agi::fs::path const& filename, std::string_view colormatrix, agi::BackgroundRunner *br);

	void GetFrame(int n, VideoFrame &out) override;

	void SetColorSpace(std::string const& matrix) override {
		if (matrix == ColorSpace) return;
		// The HDR mapper is constructed for the source matrix. Applying a saved
		// SDR TV.601 override would make swscale feed it unrelated RGB values.
		if (UseCpuToneMap) {
			LOG_W("video/provider/ffmpegsource")
				<< "Ignoring manual colour-matrix override '" << matrix
				<< "' while HDR CPU tone mapping is active";
			return;
		}
		if (matrix == RealColorSpace)
			FFMS_SetInputFormatV(VideoSource, CS, CR, FFMS_GetPixFmt(""), nullptr);
		else if (matrix == "TV.601")
			FFMS_SetInputFormatV(VideoSource, AGI_CS_BT470BG, CR, FFMS_GetPixFmt(""), nullptr);
		else
			return;
		ColorSpace = matrix;
	}

	int GetFrameCount() const override             { return VideoInfo->NumFrames; }

	int GetWidth() const override  { auto rotation = normalize_rotation(VideoInfo->Rotation); return (rotation == 90 || rotation == 270) ? Height : Width; }
	int GetHeight() const override { auto rotation = normalize_rotation(VideoInfo->Rotation); return (rotation == 90 || rotation == 270) ? Width : Height; }
	double GetDAR() const override { auto rotation = normalize_rotation(VideoInfo->Rotation); return (rotation == 90 || rotation == 270) ? 1 / DAR : DAR; }

	agi::vfr::Framerate GetFPS() const override    { return Timecodes; }
	std::string GetColorSpace() const override     { return ColorSpace; }
	std::string GetRealColorSpace() const override { return RealColorSpace; }
	std::vector<int> GetKeyFrames() const override { return KeyFramesList; };
	std::string GetDecoderName() const override    { return "FFmpegSource"; }
	bool WantsCaching() const override             { return true; }
	bool HasAudio() const override                 { return has_audio; }
};

std::string colormatrix_description(int cs, int cr) {
	// Assuming TV for unspecified
	std::string str = cr == FFMS_CR_JPEG ? "PC" : "TV";

	switch (cs) {
		case AGI_CS_RGB:
			return "None";
		case AGI_CS_BT709:
			return str + ".709";
		case AGI_CS_FCC:
			return str + ".FCC";
		case AGI_CS_BT470BG:
		case AGI_CS_SMPTE170M:
			return str + ".601";
		case AGI_CS_SMPTE240M:
			return str + ".240M";
		case AGI_CS_BT2020_NCL:
		case AGI_CS_BT2020_CL:
			// BT.2020 is used by UHD/4K HDR content. Aegisub's resolution
			// resampler has no native BT.2020 matrix, so downstream it is
			// treated the same as BT.709, but it must still produce a name
			// here so the video can be opened instead of being rejected as
			// an "unknown color space".
			return str + ".2020";
		case AGI_CS_UNSPECIFIED:
			// Handled by the caller (defaulted before we get here), but keep
			// an explicit branch so future colorspaces are caught explicitly.
			return str + ".709";
		default:
			// YCOCG, SMPTE2085, chromaticity-derived, ICTcp and anything else
			// libavutil may report. Rather than refusing to open the video,
			// expose a usable UI label, but do not hide that this is approximate.
			LOG_W("video/provider/ffmpegsource") << "Video reports unsupported colour-space id "
				<< cs << "; preview conversion and the .709 label may be approximate";
			return str + ".709";
	}
}

FFmpegSourceVideoProvider::FFmpegSourceVideoProvider(agi::fs::path const& filename, std::string_view colormatrix, agi::BackgroundRunner *br) try
: FFmpegSourceProvider(br)
, VideoSource(nullptr, FFMS_DestroyVideoSource)
{
	ErrInfo.Buffer		= FFMSErrMsg;
	ErrInfo.BufferSize	= sizeof(FFMSErrMsg);
	ErrInfo.ErrorType	= FFMS_ERROR_SUCCESS;
	ErrInfo.SubType		= FFMS_ERROR_SUCCESS;

	SetLogLevel();

	LoadVideo(filename, colormatrix);
}
catch (agi::EnvironmentError const& err) {
	throw VideoOpenError(err.GetMessage());
}

void FFmpegSourceVideoProvider::LoadVideo(agi::fs::path const& filename, std::string_view colormatrix) {
	FFMS_Indexer *Indexer = FFMS_CreateIndexer(filename.string().c_str(), &ErrInfo);
	if (!Indexer) {
		if (ErrInfo.SubType == FFMS_ERROR_FILE_READ)
			throw agi::fs::FileNotFound(std::string(ErrInfo.Buffer));
		else
			throw VideoNotSupported(ErrInfo.Buffer);
	}

	std::map<int, std::string> TrackList = GetTracksOfType(Indexer, FFMS_TYPE_VIDEO);
	if (TrackList.size() <= 0)
		throw VideoNotSupported("no video tracks found");

	int TrackNumber = -1;
	if (TrackList.size() > 1) {
		auto Selection = AskForTrackSelection(TrackList, FFMS_TYPE_VIDEO);
		if (Selection == TrackSelection::None)
			throw agi::UserCancelException("video loading cancelled by user");
		TrackNumber = static_cast<int>(Selection);
	}

	// generate a name for the cache file
	auto CacheName = GetCacheFilename(filename);

	// try to read index
	agi::scoped_holder<FFMS_Index*, void (FFMS_CC*)(FFMS_Index*)>
		Index(FFMS_ReadIndex(CacheName.string().c_str(), &ErrInfo), FFMS_DestroyIndex);

	if (Index && FFMS_IndexBelongsToFile(Index, filename.string().c_str(), &ErrInfo))
		Index = nullptr;

	// time to examine the index and check if the track we want is indexed
	// technically this isn't really needed since all video tracks should always be indexed,
	// but a bit of sanity checking never hurt anyone
	if (Index && TrackNumber >= 0) {
		FFMS_Track *TempTrackData = FFMS_GetTrackFromIndex(Index, TrackNumber);
		if (FFMS_GetNumFrames(TempTrackData) <= 0)
			Index = nullptr;
	}

	// moment of truth
	if (!Index) {
		auto TrackMask = TrackSelection::None;
		if (OPT_GET("Provider/FFmpegSource/Index All Tracks")->GetBool() || OPT_GET("Video/Open Audio")->GetBool())
			TrackMask = TrackSelection::All;
		Index = DoIndexing(Indexer, CacheName, TrackMask, GetErrorHandlingMode());
	}
	else {
		FFMS_CancelIndexing(Indexer);
	}

	// update access time of index file so it won't get cleaned away
	agi::fs::Touch(CacheName);

	// we have now read the index and may proceed with cleaning the index cache
	CleanCache();

	// track number still not set?
	if (TrackNumber < 0) {
		// just grab the first track
		TrackNumber = FFMS_GetFirstIndexedTrackOfType(Index, FFMS_TYPE_VIDEO, &ErrInfo);
		if (TrackNumber < 0)
			throw VideoNotSupported(std::string("Couldn't find any video tracks: ") + ErrInfo.Buffer);
	}

	// Check if there's an audio track
	has_audio = FFMS_GetFirstTrackOfType(Index, FFMS_TYPE_AUDIO, nullptr) != -1;

	// set thread count
	int Threads = OPT_GET("Provider/Video/FFmpegSource/Decoding Threads")->GetInt();

	// set seekmode
	// TODO: give this its own option?
	int SeekMode;
	if (OPT_GET("Provider/Video/FFmpegSource/Unsafe Seeking")->GetBool())
		SeekMode = FFMS_SEEK_UNSAFE;
	else
		SeekMode = FFMS_SEEK_NORMAL;

	VideoSource = FFMS_CreateVideoSource(filename.string().c_str(), TrackNumber, Index, Threads, SeekMode, &ErrInfo);
	if (!VideoSource)
		throw VideoOpenError(std::string("Failed to open video track: ") + ErrInfo.Buffer);

	// load video properties
	VideoInfo = FFMS_GetVideoProperties(VideoSource);

	const FFMS_Frame *TempFrame = FFMS_GetFrame(VideoSource, 0, &ErrInfo);
	if (!TempFrame)
		throw VideoOpenError(std::string("Failed to decode first frame: ") + ErrInfo.Buffer);

	Width  = TempFrame->EncodedWidth;
	Height = TempFrame->EncodedHeight;
	if (VideoInfo->SARDen > 0 && VideoInfo->SARNum > 0)
		DAR = double(Width) * VideoInfo->SARNum / ((double)Height * VideoInfo->SARDen);
	else
		DAR = double(Width) / Height;

	int VideoCS = CS = TempFrame->ColorSpace;
	CR = TempFrame->ColorRange;

	// Capture HDR metadata so the standard HDR10/HLG base layer can be converted
	// by the preview-grade CPU mapper. Dolby Vision IPT/ICtCp profiles are kept
	// out of this path: their nonlinear RPU reshaping cannot be replaced by a
	// fixed matrix.
	// FFMS2's field name is intentionally misspelled in the upstream header
	// ("TransferCharateristics"); keep the typo to match.
	Transfer = TempFrame->TransferCharateristics;
	Primaries = TempFrame->ColorPrimaries;
	// FFMS_Frame exposes flat fields (not a nested struct): ContentLightLevelMax.
	MaxCLL = TempFrame->HasContentLightLevel ? static_cast<int>(TempFrame->ContentLightLevelMax) : 0;
	HasDolbyVision = TempFrame->DolbyVisionRPUSize > 0;
	IsHDR = HDRTonemap::IsHDRSource(Transfer, Primaries) || HasDolbyVision;

	// Resolve missing matrix metadata before deciding whether rgb48le is safe.
	// The CPU mapper may only consume output from matrices libswscale actually
	// understands; treating ICTCP/IPT_C2 or a future id as ordinary RGB causes
	// severe and misleading colour errors.
	if (CS == AGI_CS_UNSPECIFIED)
		CS = Width > 1024 || Height >= 600 ? AGI_CS_BT709 : AGI_CS_BT470BG;
	bool IsDolbyVisionIpt = CS == AGI_CS_ICTCP || CS == AGI_CS_IPT_C2;
	bool HasSupportedInputMatrix = swscale_has_input_matrix(CS);
	UseCpuToneMap = HDRTonemap::IsHDRSource(Transfer, Primaries)
		&& HasSupportedInputMatrix && !IsDolbyVisionIpt;
	if (UseCpuToneMap) {
		bool convert_bt2020 = Primaries == 9 || CS == AGI_CS_BT2020_NCL || CS == AGI_CS_BT2020_CL;
		CpuToneMapper = std::make_unique<HDRTonemap::ToneMapper>(Transfer, convert_bt2020, MaxCLL);
		LOG_I("video/provider/ffmpegsource") << "HDR source detected (transfer=" << Transfer
			<< ", primaries=" << Primaries << ", maxCLL=" << MaxCLL
			<< "); using preview-grade CPU tone mapping";
	}
	if (HasDolbyVision && UseCpuToneMap)
		LOG_W("video/provider/ffmpegsource")
			<< "Dolby Vision RPU metadata detected. Preview tone mapping uses only the "
				"standard PQ/HLG base layer; RPU reshaping is not applied.";
	else if (IsDolbyVisionIpt)
		LOG_W("video/provider/ffmpegsource")
			<< "Dolby Vision IPT/ICtCp (profile 5 style) video is unsupported by the "
				"CPU tone mapper. RPU reshaping is not applied and preview colours are unreliable.";
	else if (HasDolbyVision)
		LOG_W("video/provider/ffmpegsource")
			<< "Dolby Vision metadata has no usable standard PQ/HLG base layer. "
				"RPU reshaping is not applied and preview colours are unreliable.";
	else if (HDRTonemap::IsHDRSource(Transfer, Primaries) && !HasSupportedInputMatrix)
		LOG_W("video/provider/ffmpegsource")
			<< "HDR source reports unsupported colour-space id " << CS
			<< "; disabling CPU tone mapping because libswscale cannot safely "
				"convert this matrix to RGB.";

	RealColorSpace = ColorSpace = colormatrix_description(CS, CR);

	if (!UseCpuToneMap && CS != AGI_CS_RGB && CS != AGI_CS_BT470BG
	&& ColorSpace != colormatrix && colormatrix == "TV.601") {
		CS = AGI_CS_BT470BG;
		ColorSpace = colormatrix_description(AGI_CS_BT470BG, CR);
	}
	else if (UseCpuToneMap && colormatrix == "TV.601" && ColorSpace != colormatrix)
		LOG_W("video/provider/ffmpegsource")
			<< "Ignoring saved TV.601 override while HDR CPU tone mapping is active";

	if (CS != VideoCS) {
		if (FFMS_SetInputFormatV(VideoSource, CS, CR, FFMS_GetPixFmt(""), &ErrInfo))
			throw VideoOpenError(std::string("Failed to set input format: ") + ErrInfo.Buffer);
	}

	// Standard HDR10/HLG is converted by swscale to transfer-encoded rgb48le, then
	// tone-mapped below. SDR retains the normal direct BGRA path. Dolby Vision
	// IPT/ICtCp stays on BGRA because treating those nonlinear channels as RGB
	// would be actively misleading.
	const int bgra_fmt = FFMS_GetPixFmt("bgra");
	const int rgb48_fmt = FFMS_GetPixFmt("rgb48le");
	const int TargetFormat[] = { UseCpuToneMap ? rgb48_fmt : bgra_fmt, -1 };

	// Performance: convert large HDR sources to a 1920-wide preview after codec
	// decoding. This reduces swscale, upload and frame-cache costs; it does not
	// reduce the codec's own full-resolution decode work.
	int out_w = Width;
	int out_h = Height;
	if (IsHDR && Width > 1920) {
		const int preview_max_width = 1920;
		double scale = static_cast<double>(preview_max_width) / Width;
		out_w = preview_max_width;
		out_h = std::max(2, static_cast<int>(Height * scale) & ~1);
		LOG_I("video/provider/ffmpegsource") << "HDR preview downscaled from "
			<< Width << "x" << Height << " to " << out_w << "x" << out_h
			<< " to keep playback responsive";
	}

	int resizer = IsHDR ? FFMS_RESIZER_BILINEAR : FFMS_RESIZER_BICUBIC;
	if (FFMS_SetOutputFormatV2(VideoSource, TargetFormat, out_w, out_h, resizer, &ErrInfo))
		throw VideoOpenError(std::string("Failed to set output format: ") + ErrInfo.Buffer);

	if (IsHDR && Width > 1920) {
		Width = out_w;
		Height = out_h;
	}

	// get frame info data
	FFMS_Track *FrameData = FFMS_GetTrackFromVideo(VideoSource);
	if (FrameData == nullptr)
		throw VideoOpenError("failed to get frame data");
	const FFMS_TrackTimeBase *TimeBase = FFMS_GetTimeBase(FrameData);
	if (TimeBase == nullptr)
		throw VideoOpenError("failed to get track time base");

	// build list of keyframes and timecodes
	std::vector<int> TimecodesVector;
	for (int CurFrameNum = 0; CurFrameNum < VideoInfo->NumFrames; CurFrameNum++) {
		const FFMS_FrameInfo *CurFrameData = FFMS_GetFrameInfo(FrameData, CurFrameNum);
		if (!CurFrameData)
			throw VideoOpenError("Couldn't get info about frame " + std::to_string(CurFrameNum));

		// keyframe?
		if (CurFrameData->KeyFrame)
			KeyFramesList.push_back(CurFrameNum);

		// calculate timestamp and add to timecodes vector
		int Timestamp = (int)((CurFrameData->PTS * TimeBase->Num) / TimeBase->Den);
		TimecodesVector.push_back(Timestamp);
	}
	if (TimecodesVector.size() < 2)
		Timecodes = 25.0;
	else
		Timecodes = agi::vfr::Framerate(TimecodesVector);
}

void FFmpegSourceVideoProvider::GetFrame(int n, VideoFrame &out) {
	n = mid(0, n, GetFrameCount() - 1);

	auto frame = FFMS_GetFrame(VideoSource, n, &ErrInfo);
	if (!frame)
		throw VideoDecodeError(std::string("Failed to retrieve frame: ") +  ErrInfo.Buffer);

	out.flipped = false;
	out.width = Width;
	out.height = Height;

	if (UseCpuToneMap) {
		out.pitch = 4 * Width;
		out.data.resize(static_cast<size_t>(out.pitch) * Height);
		CpuToneMapper->ToneMapRGB48toBGRA8(frame->Data[0], frame->Linesize[0],
			out.data.data(), out.pitch, Width, Height);
	}
	else {
		// FFMS normally returns a positive padded stride. Handle a negative or
		// unexpectedly short stride without constructing an invalid vector range.
		if (frame->Linesize[0] >= 4 * Width) {
			out.pitch = frame->Linesize[0];
			out.data.assign(frame->Data[0], frame->Data[0] + static_cast<size_t>(out.pitch) * Height);
		}
		else if (frame->Linesize[0] <= -4 * Width) {
			out.pitch = 4 * Width;
			out.data.resize(static_cast<size_t>(out.pitch) * Height);
			for (int y = 0; y < Height; ++y)
				std::copy_n(frame->Data[0] + static_cast<ptrdiff_t>(y) * frame->Linesize[0],
					out.pitch, out.data.data() + static_cast<size_t>(y) * out.pitch);
		}
		else
			throw VideoDecodeError("FFmpegSource returned an invalid BGRA row stride");
	}

	// Handle flip — use out.pitch (not frame->Linesize[0]) so this is correct
	// for both the HDR tone-mapped buffer and the raw SDR buffer.
	if (VideoInfo->Flip > 0)
		for (int x = 0; x < Height; ++x)
			for (int y = 0; y < Width / 2; ++y)
				swap_bgra(out.data.data() + out.pitch * x + 4 * y,
					out.data.data() + out.pitch * x + 4 * (Width - 1 - y));

	else if (VideoInfo->Flip < 0)
		for (int x = 0; x < Height / 2; ++x)
			for (int y = 0; y < Width; ++y)
				swap_bgra(out.data.data() + out.pitch * x + 4 * y,
					out.data.data() + out.pitch * (Height - 1 - x) + 4 * y);

	// Handle rotation
	auto rotation = normalize_rotation(VideoInfo->Rotation);
	if (rotation == 180) {
		std::vector<unsigned char> data(std::move(out.data));
		out.data.resize(Width * Height * 4);
		for (int x = 0; x < Height; ++x)
			for (int y = 0; y < Width; ++y)
				copy_bgra(out.data.data() + 4 * (Width * x + y),
					data.data() + out.pitch * (Height - 1 - x) + 4 * (Width - 1 - y));
		out.pitch = 4 * Width;
	}
	else if (rotation == 90) {
		std::vector<unsigned char> data(std::move(out.data));
		out.data.resize(Width * Height * 4);
		for (int x = 0; x < Width; ++x)
			for (int y = 0; y < Height; ++y)
				copy_bgra(out.data.data() + 4 * (Height * x + y),
					data.data() + out.pitch * y + 4 * (Width - 1 - x));
		out.width = Height;
		out.height = Width;
		out.pitch = 4 * Height;
	}
	else if (rotation == 270) {
		std::vector<unsigned char> data(std::move(out.data));
		out.data.resize(Width * Height * 4);
		for (int x = 0; x < Width; ++x)
			for (int y = 0; y < Height; ++y)
				copy_bgra(out.data.data() + 4 * (Height * x + y),
					data.data() + out.pitch * (Height - 1 - y) + 4 * x);
		out.width = Height;
		out.height = Width;
		out.pitch = 4 * Height;
	}
}
}

std::unique_ptr<VideoProvider> CreateFFmpegSourceVideoProvider(agi::fs::path const& path, std::string_view colormatrix, agi::BackgroundRunner *br) {
	return std::make_unique<FFmpegSourceVideoProvider>(path, colormatrix, br);
}

#endif /* WITH_FFMS2 */
