// Copyright (c) 2009, Amar Takhar <verm@aegisub.org>
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

#include "libresrc.h"

#include "../theme.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

#include <wx/bitmap.h>
#include <wx/bmpbndl.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/image.h>
#include <wx/intl.h>
#include <wx/mstream.h>

namespace {
void adapt_image_to_dark_theme(wxImage& image) {
	if (!image.IsOk()) return;

	auto data = image.GetData();
	auto alpha = image.HasAlpha() ? image.GetAlpha() : nullptr;
	auto pixels = static_cast<size_t>(image.GetWidth()) * image.GetHeight();
	constexpr double cutoff = 192.0;
	constexpr double target = 224.0;
	constexpr double strength = 0.82;

	for (size_t pixel = 0; pixel < pixels; ++pixel) {
		if (alpha && alpha[pixel] == 0) continue;

		auto rgb = data + pixel * 3;
		double value = std::max({rgb[0], rgb[1], rgb[2]});
		if (value >= cutoff) continue;

		// Raise only dark colours. Scaling all channels equally preserves hue,
		// while the original alpha keeps the source image's antialiased edges.
		double amount = strength * (cutoff - value) / cutoff;
		double new_value = value + (target - value) * amount;
		if (value == 0.0) {
			auto grey = static_cast<unsigned char>(std::lround(new_value));
			rgb[0] = rgb[1] = rgb[2] = grey;
		}
		else {
			double scale = new_value / value;
			for (int channel = 0; channel < 3; ++channel)
				rgb[channel] = static_cast<unsigned char>(std::lround(rgb[channel] * scale));
		}
	}
}
}

wxBitmap libresrc_getimage(const unsigned char *buff, size_t size, int dir, bool adapt_to_dark_theme) {
	wxMemoryInputStream mem(buff, size);
	wxImage image(mem);
	if (adapt_to_dark_theme && app_theme::IsDark())
		adapt_image_to_dark_theme(image);
	if (dir == wxLayout_RightToLeft)
		image = image.Mirror();
	return wxBitmap(image);
}

wxIcon libresrc_geticon(const unsigned char *buff, size_t size) {
	wxIcon icon;
	icon.CopyFromBitmap(libresrc_getimage(buff, size));
	return icon;
}

wxBitmapBundle libresrc_getbitmapbundle(const LibresrcBlob *images, size_t count, int height, int dir) {
	// This function should only ever be called on the GUI thread but declaring this thread_local is the safe way
	thread_local std::map<std::tuple<const LibresrcBlob *, int, int, bool>, wxBitmapBundle> cache;
	auto key = std::make_tuple(images, height, dir, app_theme::IsDark());

	if (auto cached = cache.find(key); cached != cache.end()) {
		return cached->second;
	}

	wxVector<wxBitmap> bitmaps;
	bitmaps.reserve(count);
	for (size_t i = 0; i < count; i++) {
		bitmaps.push_back(libresrc_getimage(images[i].data, images[i].size, dir));
		bitmaps.back().SetScaleFactor(double(images[i].scale) / height);
	}

	auto bundle = wxBitmapBundle::FromBitmaps(bitmaps);
	cache[key] = bundle;

	return bundle;
}

wxIconBundle libresrc_geticonbundle(const LibresrcBlob *images, size_t count) {
	thread_local std::map<std::pair<const LibresrcBlob *, bool>, wxIconBundle> cache;
	auto key = std::make_pair(images, app_theme::IsDark());

	if (auto cached = cache.find(key); cached != cache.end()) {
		return cached->second;
	}

	wxIconBundle bundle;
	for (size_t i = 0; i < count; i++) {
		bundle.AddIcon(libresrc_geticon(images[i].data, images[i].size));
	}

	cache[key] = bundle;

	return bundle;
}
