// Copyright (c) 2005, Rodrigo Braz Monteiro
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

#include "video_box.h"

#include "ass_dialogue.h"
#include "ass_file.h"
#include "async_video_provider.h"
#include "compat.h"
#include "format.h"
#include "include/aegisub/context.h"
#include "include/aegisub/toolbar.h"
#include "options.h"
#include "project.h"
#include "selection_controller.h"
#include "theme.h"
#include "video_controller.h"
#include "video_display.h"
#include "video_slider.h"

#include <boost/range/algorithm/binary_search.hpp>
#include <wx/combobox.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/statline.h>
#include <wx/textctrl.h>
#include <wx/toolbar.h>

VideoBox::VideoBox(wxWindow *parent, bool isDetached,
	VisualToolPreviewBar *previewBar, agi::Context *context)
: wxPanel(parent, -1)
, context(context)
{
	auto videoSlider = new VideoSlider(this, context);
	videoSlider->SetToolTip(_("Seek video"));

	auto mainToolbar = toolbar::GetToolbar(this, "video", context, "Video", false);

	VideoPosition = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxSize(110, -1), wxTE_READONLY);
	VideoPosition->SetToolTip(_("Current frame time and number"));

	VideoSubsPos = new wxTextCtrl(this, -1, "", wxDefaultPosition, wxSize(110, -1), wxTE_READONLY);
	VideoSubsPos->SetToolTip(_("Time of this frame relative to start and end of current subs"));

	wxArrayString choices;
	for (int i = 1; i <= 24; ++i)
		choices.Add(fmt_wx("%g%%", i * 12.5));
	auto zoomBox = new wxComboBox(this, -1, "75%", wxDefaultPosition, wxDefaultSize, choices, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);

	auto visualToolBar = toolbar::GetToolbar(this, "visual_tools", context, "Video", true);
	auto visualSubToolBar = new wxToolBar(this, -1, wxDefaultPosition, wxDefaultSize, wxTB_VERTICAL | wxTB_BOTTOM | wxTB_NODIVIDER | wxTB_FLAT);

	wxArrayString speedChoices;
	double speeds[] = {0.25, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 3, 4, 5, 6, 8, 10};
	for (double s : speeds)
		speedChoices.Add(fmt_wx("%gx", s));

	auto speedBox = new wxComboBox(this, -1, "1x", wxDefaultPosition, wxDefaultSize, speedChoices, wxCB_READONLY);

	// Next to the two buttons that decide whether subtitles and masks are drawn at
	// all: this one decides how strongly. It fades the rendering, so nothing in the
	// file changes and every line keeps its own alpha tags.
	subsOpacitySlider = new wxSlider(this, -1,
		AsyncVideoProvider::GetDisplaySubtitlesOpacity(), 0, 100,
		wxDefaultPosition, wxSize(50, -1), wxSL_HORIZONTAL);
	UpdateSubsOpacityTooltip();
	subsOpacitySlider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { ApplySubsOpacity(); });
	subsOpacitySlider->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent&) {
		subsOpacitySlider->SetValue(100);
		ApplySubsOpacity();
	});

	auto brightnessSlider = new wxSlider(this, -1, 100, 0, 400, wxDefaultPosition, wxSize(50, -1), wxSL_HORIZONTAL);
	brightnessSlider->SetToolTip(fmt_tl("Brightness: %d%%", 100) + " (" + _("Right click to reset") + ")");

	auto videoDisplay = new VideoDisplay(visualSubToolBar, isDetached, zoomBox, speedBox,
		brightnessSlider, previewBar, this, context);
	videoDisplay->MoveBeforeInTabOrder(videoSlider);

	auto toolbarSizer = new wxBoxSizer(wxVERTICAL);
	toolbarSizer->Add(visualToolBar, wxSizerFlags(1));
	toolbarSizer->Add(visualSubToolBar, wxSizerFlags());

	auto topSizer = new wxBoxSizer(wxHORIZONTAL);
	topSizer->Add(toolbarSizer, 0, wxEXPAND);
	topSizer->Add(videoDisplay, isDetached, isDetached ? wxEXPAND : 0);

	auto videoBottomSizer = new wxBoxSizer(wxHORIZONTAL);
	videoBottomSizer->Add(mainToolbar, wxSizerFlags(0).Center());
	videoBottomSizer->Add(subsOpacitySlider, wxSizerFlags(0).Center());
	videoBottomSizer->Add(VideoPosition, wxSizerFlags(1).Center().Border(wxLEFT));
	videoBottomSizer->Add(VideoSubsPos, wxSizerFlags(1).Center().Border(wxLEFT));
	videoBottomSizer->Add(speedBox, wxSizerFlags(0).Center().Border(wxLEFT));
	videoBottomSizer->Add(brightnessSlider, wxSizerFlags(0).Center().Border(wxLEFT, 0));
	videoBottomSizer->Add(zoomBox, wxSizerFlags(0).Center().Border(wxLEFT, 0));

	auto VideoSizer = new wxBoxSizer(wxVERTICAL);
	VideoSizer->Add(topSizer, 1, wxEXPAND, 0);
	VideoSizer->Add(new wxStaticLine(this), 0, wxEXPAND, 0);
	VideoSizer->Add(videoSlider, 0, wxEXPAND, 0);
	VideoSizer->Add(videoBottomSizer, 0, wxEXPAND | wxBOTTOM, 5);
	SetSizer(VideoSizer);

	UpdateTimeBoxes();

	connections = agi::signal::make_vector({
		context->ass->AddCommitListener(&VideoBox::UpdateTimeBoxes, this),
		context->project->AddKeyframesListener(&VideoBox::UpdateTimeBoxes, this),
		context->project->AddTimecodesListener(&VideoBox::UpdateTimeBoxes, this),
		context->project->AddVideoProviderListener(&VideoBox::UpdateTimeBoxes, this),
		context->selectionController->AddSelectionListener(&VideoBox::UpdateTimeBoxes, this),
		context->videoController->AddSeekListener(&VideoBox::UpdateTimeBoxes, this),
	});
}

void VideoBox::UpdateSubsOpacityTooltip() {
	subsOpacitySlider->SetToolTip(
		fmt_tl("Subtitle opacity: %d%%", subsOpacitySlider->GetValue()) +
		" (" + _("Right click to reset") + ")");
}

void VideoBox::ApplySubsOpacity() {
	UpdateSubsOpacityTooltip();

	int value = subsOpacitySlider->GetValue();
	if (AsyncVideoProvider::GetDisplaySubtitlesOpacity() == value) return;
	AsyncVideoProvider::SetDisplaySubtitlesOpacity(value);

	// The subtitles are drawn into the frame by the provider, so the frame on screen
	// has to be made again rather than merely redrawn.
	if (auto provider = context->project->VideoProvider())
		provider->ResetCurrentFrame();
}

void VideoBox::UpdateTimeBoxes() {
	if (!context->project->VideoProvider()) return;

	int frame = context->videoController->GetFrameN();
	int time = context->videoController->TimeAtFrame(frame, agi::vfr::EXACT);

	// Set the text box for frame number and time
	VideoPosition->SetValue(fmt_wx("%s - %d", agi::Time(time).GetAssFormatted(true), frame));
	if (boost::binary_search(context->project->Keyframes(), frame)) {
		// Set the background color to indicate this is a keyframe
		VideoPosition->SetBackgroundColour(app_theme::Colour("Subtitle Grid/Background/Selection"));
		VideoPosition->SetForegroundColour(app_theme::Colour("Subtitle Grid/Selection"));
	}
	else {
		VideoPosition->SetBackgroundColour(wxNullColour);
		VideoPosition->SetForegroundColour(wxNullColour);
	}

	AssDialogue *active_line = context->selectionController->GetActiveLine();
	if (!active_line)
		VideoSubsPos->SetValue("");
	else {
		VideoSubsPos->SetValue(fmt_wx(
			"%+dms; %+dms",
			time - active_line->Start,
			time - active_line->End));
	}
}
