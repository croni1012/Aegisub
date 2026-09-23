// Copyright (c) 2005-2010, Niels Martin Hansen
// Copyright (c) 2005-2010, Rodrigo Braz Monteiro
// Copyright (c) 2010, Amar Takhar
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

#include "command.h"

#include "../help_button.h"
#include "../include/aegisub/context.h"
#include "../libresrc/libresrc.h"


#include <wx/msgdlg.h>

namespace {
	using cmd::Command;

struct help_bugs final : public Command {
	CMD_NAME("help/bugs")
	CMD_ICON(bugtracker_button)
	STR_MENU("&Bug Tracker...")
	STR_DISP("Bug Tracker")
	STR_HELP("Visit Aegisub's bug tracker to report bugs and request new features")

	void operator()(agi::Context *c) override {
		if (wxGetMouseState().CmdDown()) {
			if (wxGetMouseState().ShiftDown()) {
				 wxMessageBox("Now crashing with an access violation...");
				for (char *foo = (char*)nullptr;;) *foo++ = 42;
			} else {
				wxMessageBox("Now crashing with an unhandled exception...");
				throw c->parent;
			}
		}
		wxLaunchDefaultBrowser("https://github.com/TypesettingTools/Aegisub/issues", wxBROWSER_NEW_WINDOW);
	}
};

struct help_contents final : public Command {
	CMD_NAME("help/contents")
	CMD_ICON(contents_button)
	STR_MENU("&Contents")
	STR_DISP("Contents")
	STR_HELP("Help topics")

	void operator()(agi::Context *) override {
		HelpButton::OpenPage("Main");
	}
};

struct help_irc final : public Command {
	CMD_NAME("help/irc")
	CMD_ICON(irc_button)
	STR_MENU("&IRC Channel")
	STR_DISP("IRC Channel")
	STR_HELP("Visit Aegisub's official IRC channel")

	void operator()(agi::Context *) override {
		wxLaunchDefaultBrowser("irc://irc.rizon.net/aegisub", wxBROWSER_NEW_WINDOW);
	}
};

struct help_video final : public Command {
	CMD_NAME("help/video")
	CMD_ICON(visual_help)
	STR_MENU("&Visual Typesetting")
	STR_DISP("Visual Typesetting")
	STR_HELP("Open the visual typesetting guides")

	void operator()(agi::Context *) override {
		// Both, rather than one with the other as a fallback. The illustrated guide is
		// the more useful of the two, but it is hosted outside the project and may not
		// answer at all one day; a fallback nobody sees would leave the button doing
		// nothing on that day, so the official manual is opened alongside it.
		wxLaunchDefaultBrowser("https://aegisub.org/docs/latest/visual_typesetting/",
			wxBROWSER_NEW_WINDOW);
		// Opened second so it is the page in front, being the one with the pictures.
		wxLaunchDefaultBrowser("https://kintsugi-fansub.hu/public/nyaa/aegisub/docs/?lang=en",
			wxBROWSER_NEW_WINDOW);
	}
};

struct help_website final : public Command {
	CMD_NAME("help/website")
	CMD_ICON(website_button)
	STR_MENU("&Website")
	STR_DISP("Website")
	STR_HELP("Visit Aegisub's official website")

	void operator()(agi::Context *) override {
		wxLaunchDefaultBrowser("https://aegisub.org/", wxBROWSER_NEW_WINDOW);
	}
};

struct help_kintsugi final : public Command {
	CMD_NAME("help/kintsugi")
	CMD_ICON(contents_button)
	STR_MENU("&Documentation...")
	STR_DISP("Kintsugi Fansub Documentation")
	STR_HELP("Open the Kintsugi Fansub Aegisub documentation")
	void operator()(agi::Context *) override {
		wxLaunchDefaultBrowser("https://kintsugi-fansub.hu/public/nyaa/aegisub/docs/", wxBROWSER_NEW_WINDOW);
	}
};
}

namespace cmd {
	void init_help() {
		reg(std::make_unique<help_kintsugi>());
		reg(std::make_unique<help_bugs>());
		reg(std::make_unique<help_contents>());
		reg(std::make_unique<help_irc>());
		reg(std::make_unique<help_video>());
		reg(std::make_unique<help_website>());
	}
}
