// Copyright (c) 2026, arch1t3cht <arch1t3cht@gmail.com>
// Copyright (c) 2012 Thomas Goyne, <plorkyeran@aegisub.org>
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
// Aegisub Project https://aegisub.org/

#include <libaegisub/scoped_ptr.h>

#include <wx/dcmemory.h>
#include <wx/thread.h>

#include <algorithm>

#import <ApplicationServices/ApplicationServices.h>
#import <CoreGraphics/CGDirectDisplay.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

namespace osx {

namespace {

void CopyToDC(CGImageRef img, wxMemoryDC &capdc, int resx, int resy, int magnification) {
	if (!img || resx <= 0 || resy <= 0) return;
	std::vector<uint8_t> imgdata(static_cast<size_t>(resy) * resx * 4);

	agi::scoped_holder<CGColorSpaceRef> colorspace(CGColorSpaceCreateDeviceRGB(), CGColorSpaceRelease);
	agi::scoped_holder<CGContextRef> bmp_context(CGBitmapContextCreate(
		imgdata.data(), resx, resy, 8, 4 * resx, colorspace,
		kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big), CGContextRelease);

	if (!bmp_context) return;
	CGContextSetInterpolationQuality(bmp_context, kCGInterpolationNone);
	CGContextDrawImage(bmp_context, CGRectMake(0, 0, resx, resy), img);

	for (int x = 0; x < resx; x++) {
		for (int y = 0; y < resy; y++) {
			uint8_t *pixel = &imgdata[(y * resx + x) * 4];
			capdc.SetBrush(wxBrush(wxColour(pixel[0], pixel[1], pixel[2])));
			capdc.DrawRectangle(x * magnification, y * magnification, magnification, magnification);
		}
	}
}

}

void DropFromScreen(int x, int y, int resx, int resy, int magnification, wxMemoryDC &capdc) {
	CGRect rect = CGRectMake(x - resx / 2., y - resy / 2., resx, resy);

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150200
	if (@available(macOS 15.2, *)) {
		wxSemaphore screenshot_notify{0, 1};
		wxSemaphore &screenshot_notify_ref = screenshot_notify;
		__block CGImageRef captured = nullptr;

		[SCScreenshotManager captureImageInRect:rect completionHandler:^(CGImageRef _Nullable image, NSError * _Nullable error) {
			if (!error && image)
				captured = CGImageRetain(image);

			screenshot_notify_ref.Post();
		}];

		screenshot_notify.Wait();
		if (captured) {
			CopyToDC(captured, capdc, resx, resy, magnification);
			CGImageRelease(captured);
		}
		return;
	}
#endif

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
	if (@available(macOS 14.0, *)) {
		wxSemaphore screenshot_notify{0, 1};
		wxSemaphore &screenshot_notify_ref = screenshot_notify;
		__block CGImageRef captured = nullptr;

		rect.origin.x = std::roundl(rect.origin.x);
		rect.origin.y = std::roundl(rect.origin.y);

		[SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent * _Nullable shareableContent, NSError * _Nullable error) {
			if (error) {
				screenshot_notify_ref.Post();
				return;
			}

			CGDirectDisplayID display_id = 0;
			uint32_t display_count = 0;
			if (CGGetDisplaysWithPoint(CGPointMake(x, y), 1, &display_id, &display_count) != kCGErrorSuccess
				|| display_count == 0) {
				screenshot_notify_ref.Post();
				return;
			}

			CGDisplayModeRef displaymode = CGDisplayCopyDisplayMode(display_id);
			if (!displaymode) {
				screenshot_notify_ref.Post();
				return;
			}
			auto mode_width = CGDisplayModeGetWidth(displaymode);
			int scale_factor = mode_width ? std::max<size_t>(1, CGDisplayModeGetPixelWidth(displaymode) / mode_width) : 1;
			CGDisplayModeRelease(displaymode);

			SCDisplay* point_display = nullptr;

			for (SCDisplay *display in shareableContent.displays) {
				if (display.displayID == display_id) {
					point_display = display;
				}
			}

			if (!point_display) {
				screenshot_notify_ref.Post();
				return;
			}

			CGRect display_bounds = CGDisplayBounds(display_id);
			CGRect local_rect = CGRectOffset(rect, -display_bounds.origin.x, -display_bounds.origin.y);

			SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:point_display excludingWindows:@[]];
			SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
			configuration.capturesAudio = NO;
			configuration.showsCursor = NO;
			configuration.captureDynamicRange = SCCaptureDynamicRangeSDR;
			configuration.sourceRect = local_rect;
			configuration.width = resx * scale_factor;
			configuration.height = resy * scale_factor;

			[SCScreenshotManager captureImageWithFilter:filter configuration:configuration completionHandler:^(CGImageRef _Nullable sampleBuffer, NSError * _Nullable error) {
				if (!error && sampleBuffer)
					captured = CGImageRetain(sampleBuffer);

				screenshot_notify_ref.Post();
			}];

			// No ARC in this project; the async API keeps its own references for
			// the duration of the capture, so drop ours now instead of leaking.
			[filter release];
			[configuration release];
		}];

		screenshot_notify.Wait();
		if (captured) {
			CopyToDC(captured, capdc, resx, resy, magnification);
			CGImageRelease(captured);
		}
		return;
	}
#endif

#if MAC_OS_X_VERSION_MIN_REQUIRED < 150000
	// Doesn't bother handling the case where the rect overlaps two monitors
	CGDirectDisplayID display_id = 0;
	uint32_t display_count = 0;
	if (CGGetDisplaysWithPoint(CGPointMake(x, y), 1, &display_id, &display_count) != kCGErrorSuccess
		|| display_count == 0) return;

	CGRect display_bounds = CGDisplayBounds(display_id);
	CGRect local_rect = CGRectOffset(rect, -display_bounds.origin.x, -display_bounds.origin.y);
	agi::scoped_holder<CGImageRef> img(CGDisplayCreateImageForRect(display_id, local_rect), CGImageRelease);
	// Capture can fail (asleep display, denied permission); a NULL image
	// would crash CGImageGetWidth.
	if (img)
		CopyToDC(img, capdc, resx, resy, magnification);
#endif
}

}
