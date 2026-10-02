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

bool get_dark_bundle(const LibresrcBlob *images, size_t count,
	const LibresrcBlob *&dark_images, size_t &dark_count)
{
	if (!images || count == 0) return false;

#define DARK_BUNDLE(name) \
	if (images[0].data == name[0].data) { \
		dark_images = dark_##name; \
		dark_count = sizeof(dark_##name) / sizeof(*dark_##name); \
		return true; \
	}
#include "dark_bitmap_overrides.inc"
#undef DARK_BUNDLE

	return false;
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

wxIcon libresrc_geticon(const unsigned char *buff, size_t size, bool adapt_to_dark_theme) {
	wxIcon icon;
	icon.CopyFromBitmap(libresrc_getimage(buff, size, 0, adapt_to_dark_theme));
	return icon;
}

wxBitmapBundle libresrc_getbitmapbundle(const LibresrcBlob *images, size_t count, int height, int dir) {
	// This function should only ever be called on the GUI thread but declaring this thread_local is the safe way
	thread_local std::map<std::tuple<const LibresrcBlob *, int, int, bool>, wxBitmapBundle> cache;
	auto key = std::make_tuple(images, height, dir, app_theme::IsDark());

	if (auto cached = cache.find(key); cached != cache.end()) {
		return cached->second;
	}

	auto selected_images = images;
	auto selected_count = count;
	bool custom_dark = app_theme::IsDark() && get_dark_bundle(images, count, selected_images, selected_count);

	wxVector<wxBitmap> bitmaps;
	bitmaps.reserve(selected_count);
	for (size_t i = 0; i < selected_count; i++) {
		bitmaps.push_back(libresrc_getimage(selected_images[i].data, selected_images[i].size, dir, !custom_dark));
		bitmaps.back().SetScaleFactor(double(selected_images[i].scale) / height);
	}

	// A bundle containing only a large source bitmap prefers its physical size
	// even when its scale factor describes a smaller icon. Supply smaller DPI
	// variants as well so menus and toolbars can select an appropriate size.
	if (!bitmaps.empty()) {
		auto smallest = std::min_element(bitmaps.begin(), bitmaps.end(),
			[](wxBitmap const& a, wxBitmap const& b) { return a.GetHeight() < b.GetHeight(); });
		if (smallest->GetHeight() > height) {
			auto image = smallest->ConvertToImage();
			for (double scale : {1.0, 1.5, 2.0, 3.0}) {
				int target_height = static_cast<int>(std::lround(height * scale));
				if (target_height >= image.GetHeight()) break;
				int target_width = std::max(1, static_cast<int>(std::lround(
					double(image.GetWidth()) * target_height / image.GetHeight())));
				wxBitmap bitmap(image.Scale(target_width, target_height, wxIMAGE_QUALITY_HIGH));
				bitmap.SetScaleFactor(double(target_height) / height);
				bitmaps.push_back(std::move(bitmap));
			}
		}
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

	auto selected_images = images;
	auto selected_count = count;
	bool custom_dark = app_theme::IsDark() && get_dark_bundle(images, count, selected_images, selected_count);

	wxIconBundle bundle;
	for (size_t i = 0; i < selected_count; i++) {
		bundle.AddIcon(libresrc_geticon(selected_images[i].data, selected_images[i].size, !custom_dark));
	}

	cache[key] = bundle;

	return bundle;
}
