// Copyright (c) 2026
// SPDX-License-Identifier: BSD-3-Clause

#include "dialog_ai_connection.h"

#include "ai_client.h"
#include "compat.h"
#include "options.h"

#include <libaegisub/option.h>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dialog.h>
#include <wx/hyperlink.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

namespace {

class AIConnectionDialog final : public wxDialog {
	wxTextCtrl *openai_key;
	wxTextCtrl *api_base_url;
	wxTextCtrl *model;
	wxTextCtrl *image_model;
	wxCheckBox *remember_openai;
	wxStaticText *openai_status;

	wxTextCtrl *cloud_name;
	wxTextCtrl *cloud_api_key;
	wxTextCtrl *cloud_secret;
	wxCheckBox *remember_cloudinary;
	wxStaticText *cloudinary_status;
	bool require_key;

	std::string EnteredOrSavedOpenAIKey() const {
		auto entered = from_wx(openai_key->GetValue());
		return entered.empty() ? ai::GetApiKey() : entered;
	}

	ai::CloudinaryCredentials EnteredOrSavedCloudinary() const {
		auto secret = from_wx(cloud_secret->GetValue());
		if (secret.empty()) secret = ai::GetCloudinarySecret();
		return {from_wx(cloud_name->GetValue()), from_wx(cloud_api_key->GetValue()),
			std::move(secret)};
	}

	void UpdateStatus() {
		wxString openai_text;
		switch (ai::GetApiKeySource()) {
			case ai::ApiKeySource::Session:
				openai_text = _("API key is available for this Aegisub session."); break;
			case ai::ApiKeySource::Environment:
				openai_text = _("API key is provided by the OPENAI_API_KEY environment variable."); break;
			case ai::ApiKeySource::CredentialManager:
				openai_text = _("API key is stored securely in the operating system credential store."); break;
			case ai::ApiKeySource::None:
				openai_text = _("No API key is configured."); break;
		}
		openai_status->SetLabel(openai_text);
		openai_status->Wrap(360);

		wxString cloudinary_text;
		if (!ai::GetCloudinarySecret().empty())
			cloudinary_text = ai::HasStoredCloudinarySecret() ?
				_("API secret is stored securely in the operating system credential store.") :
				_("API secret is available for this Aegisub session.");
		else
			cloudinary_text = _("No Cloudinary API secret is configured.");
		cloudinary_status->SetLabel(cloudinary_text);
		cloudinary_status->Wrap(360);
		Layout();
	}

	void OnTestOpenAI(wxCommandEvent&) {
		auto key = EnteredOrSavedOpenAIKey();
		if (key.empty()) {
			wxMessageBox(_("Enter an OpenAI API key first."), _("AI connection"),
				wxOK | wxICON_WARNING, this);
			return;
		}
		wxBusyCursor busy;
		try {
			ai::OpenAIClient client(key, from_wx(model->GetValue()),
				"gpt-transcribe");
			client.TestConnection(from_wx(api_base_url->GetValue()));
			wxMessageBox(_("The OpenAI connection is working."), _("AI connection"),
				wxOK | wxICON_INFORMATION, this);
		}
		catch (std::exception const& error) {
			wxMessageBox(to_wx(error.what()), _("AI connection failed"),
				wxOK | wxICON_ERROR, this);
		}
	}

	void OnTestCloudinary(wxCommandEvent&) {
		auto credentials = EnteredOrSavedCloudinary();
		if (!credentials.Complete()) {
			wxMessageBox(_("Enter the Cloudinary cloud name, API key and API secret first."),
				_("Cloudinary connection"), wxOK | wxICON_WARNING, this);
			return;
		}
		wxBusyCursor busy;
		try {
			ai::TestCloudinaryConnection(credentials);
			wxMessageBox(_("The Cloudinary connection is working."),
				_("Cloudinary connection"), wxOK | wxICON_INFORMATION, this);
		}
		catch (std::exception const& error) {
			wxMessageBox(to_wx(error.what()), _("Cloudinary connection failed"),
				wxOK | wxICON_ERROR, this);
		}
	}

	void OnDeleteOpenAI(wxCommandEvent&) {
		if (wxMessageBox(_("Delete the OpenAI API key stored by Aegisub on this computer?"),
			_("Delete API key"), wxYES_NO | wxICON_QUESTION, this) != wxYES) return;
		std::string error;
		if (!ai::DeleteStoredApiKey(&error)) {
			wxMessageBox(to_wx(error), _("The API key could not be deleted"),
				wxOK | wxICON_ERROR, this);
			return;
		}
		openai_key->Clear();
		UpdateStatus();
	}

	void OnDeleteCloudinary(wxCommandEvent&) {
		if (wxMessageBox(_("Delete the Cloudinary API secret stored by Aegisub on this computer?"),
			_("Delete API secret"), wxYES_NO | wxICON_QUESTION, this) != wxYES) return;
		std::string error;
		if (!ai::DeleteStoredCloudinarySecret(&error)) {
			wxMessageBox(to_wx(error), _("The API secret could not be deleted"),
				wxOK | wxICON_ERROR, this);
			return;
		}
		cloud_secret->Clear();
		UpdateStatus();
	}

	void OnOK(wxCommandEvent&) {
		auto entered_openai = from_wx(openai_key->GetValue());
		if (!entered_openai.empty()) {
			std::string error;
			if (remember_openai->IsChecked()) {
				if (!ai::StoreApiKey(entered_openai, &error)) {
					wxMessageBox(to_wx(error), _("The API key could not be saved"),
						wxOK | wxICON_ERROR, this); return;
				}
			}
			else ai::SetSessionApiKey(std::move(entered_openai));
		}

		auto entered_secret = from_wx(cloud_secret->GetValue());
		if (!entered_secret.empty()) {
			std::string error;
			if (remember_cloudinary->IsChecked()) {
				if (!ai::StoreCloudinarySecret(entered_secret, &error)) {
					wxMessageBox(to_wx(error), _("The API secret could not be saved"),
						wxOK | wxICON_ERROR, this); return;
				}
			}
			else ai::SetSessionCloudinarySecret(std::move(entered_secret));
		}

		if (require_key && ai::GetApiKey().empty()) {
			wxMessageBox(_("An OpenAI API key is required for AI subtitle review."),
				_("AI connection"), wxOK | wxICON_WARNING, this);
			return;
		}

		OPT_SET("AI/OpenAI/Base URL")->SetString(from_wx(api_base_url->GetValue()));
		OPT_SET("AI/OpenAI/Model")->SetString(from_wx(model->GetValue()));
		OPT_SET("AI/OpenAI/Image Model")->SetString(from_wx(image_model->GetValue()));
		OPT_SET("AI/Cloudinary/Cloud Name")->SetString(from_wx(cloud_name->GetValue()));
		OPT_SET("AI/Cloudinary/API Key")->SetString(from_wx(cloud_api_key->GetValue()));
		config::opt->Flush();
		EndModal(wxID_OK);
	}

public:
	AIConnectionDialog(wxWindow *parent, bool require_key)
	: wxDialog(parent, wxID_ANY, _("AI connection"), wxDefaultPosition,
		wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
	, require_key(require_key) {
		auto main = new wxBoxSizer(wxVERTICAL);
		auto providers = new wxBoxSizer(wxHORIZONTAL);

		auto openai = new wxBoxSizer(wxVERTICAL);
		auto openai_title = new wxStaticText(this, wxID_ANY, _("OpenAI"));
		openai_title->SetFont(openai_title->GetFont().Bold());
		openai->Add(openai_title, wxSizerFlags().Border(wxBOTTOM, 4));
		auto openai_help = new wxStaticText(this, wxID_ANY,
			_("OpenAI is used for text features and custom image generation."));
		openai_help->Wrap(360);
		openai->Add(openai_help, wxSizerFlags().Expand().Border(wxBOTTOM, 8));
		openai_status = new wxStaticText(this, wxID_ANY, "");
		openai->Add(openai_status, wxSizerFlags().Expand().Border(wxBOTTOM, 8));
		auto openai_form = new wxFlexGridSizer(2, 8, 8);
		openai_form->AddGrowableCol(1, 1);
		openai_form->Add(new wxStaticText(this, wxID_ANY, _("New API key:")), 0, wxALIGN_CENTER_VERTICAL);
		openai_key = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
		openai_key->SetHint(_("Leave empty to keep using the current key"));
		openai_form->Add(openai_key, wxSizerFlags(1).Expand());
		openai_form->Add(new wxStaticText(this, wxID_ANY, _("API base URL:")), 0, wxALIGN_CENTER_VERTICAL);
		api_base_url = new wxTextCtrl(this, wxID_ANY,
			to_wx(OPT_GET("AI/OpenAI/Base URL")->GetString()));
		api_base_url->SetHint(to_wx(ai::DefaultApiBase()));
		openai_form->Add(api_base_url, wxSizerFlags(1).Expand());
		openai_form->AddSpacer(0);
		auto api_base_warning = new wxStaticText(this, wxID_ANY, _("Changing this could cause issues."));
		api_base_warning->Wrap(360);
		openai_form->Add(api_base_warning, wxSizerFlags(1).Expand());
		openai_form->Add(new wxStaticText(this, wxID_ANY, _("AI model:")), 0, wxALIGN_CENTER_VERTICAL);
		model = new wxTextCtrl(this, wxID_ANY, to_wx(OPT_GET("AI/OpenAI/Model")->GetString()));
		openai_form->Add(model, wxSizerFlags(1).Expand());
		openai_form->Add(new wxStaticText(this, wxID_ANY, _("Image model:")), 0, wxALIGN_CENTER_VERTICAL);
		image_model = new wxTextCtrl(this, wxID_ANY, to_wx(OPT_GET("AI/OpenAI/Image Model")->GetString()));
		openai_form->Add(image_model, wxSizerFlags(1).Expand());
		openai->Add(openai_form, wxSizerFlags().Expand().Border(wxBOTTOM, 8));
		openai->Add(new wxHyperlinkCtrl(this, wxID_ANY, _("Available OpenAI models and pricing"),
			"https://developers.openai.com/api/docs/models"), wxSizerFlags().Border(wxBOTTOM, 8));
		remember_openai = new wxCheckBox(this, wxID_ANY,
			_("Store the key securely in the operating system credential store"));
		remember_openai->SetValue(true);
		openai->Add(remember_openai, wxSizerFlags().Border(wxBOTTOM, 8));
		auto openai_actions = new wxBoxSizer(wxHORIZONTAL);
		auto test_openai = new wxButton(this, wxID_ANY, _("Test OpenAI"));
		auto delete_openai = new wxButton(this, wxID_ANY, _("Delete stored key"));
		openai_actions->Add(test_openai);
		openai_actions->Add(delete_openai, wxSizerFlags().Border(wxLEFT));
		openai->Add(openai_actions);

		auto cloudinary = new wxBoxSizer(wxVERTICAL);
		auto cloudinary_title = new wxStaticText(this, wxID_ANY, _("Cloudinary"));
		cloudinary_title->SetFont(cloudinary_title->GetFont().Bold());
		cloudinary->Add(cloudinary_title, wxSizerFlags().Border(wxBOTTOM, 4));
		auto cloudinary_help = new wxStaticText(this, wxID_ANY,
			_("Cloudinary is used for text removal and automatic recognition in clips."));
		cloudinary_help->Wrap(360);
		cloudinary->Add(cloudinary_help, wxSizerFlags().Expand().Border(wxBOTTOM, 8));
		cloudinary_status = new wxStaticText(this, wxID_ANY, "");
		cloudinary->Add(cloudinary_status, wxSizerFlags().Expand().Border(wxBOTTOM, 8));
		auto cloud_form = new wxFlexGridSizer(2, 8, 8);
		cloud_form->AddGrowableCol(1, 1);
		cloud_form->Add(new wxStaticText(this, wxID_ANY, _("Cloud name:")), 0, wxALIGN_CENTER_VERTICAL);
		cloud_name = new wxTextCtrl(this, wxID_ANY,
			to_wx(OPT_GET("AI/Cloudinary/Cloud Name")->GetString()));
		cloud_form->Add(cloud_name, wxSizerFlags(1).Expand());
		cloud_form->Add(new wxStaticText(this, wxID_ANY, _("API key:")), 0, wxALIGN_CENTER_VERTICAL);
		cloud_api_key = new wxTextCtrl(this, wxID_ANY,
			to_wx(OPT_GET("AI/Cloudinary/API Key")->GetString()));
		cloud_form->Add(cloud_api_key, wxSizerFlags(1).Expand());
		cloud_form->Add(new wxStaticText(this, wxID_ANY, _("API secret:")), 0, wxALIGN_CENTER_VERTICAL);
		cloud_secret = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);
		cloud_secret->SetHint(_("Leave empty to keep using the current secret"));
		cloud_form->Add(cloud_secret, wxSizerFlags(1).Expand());
		cloudinary->Add(cloud_form, wxSizerFlags().Expand().Border(wxBOTTOM, 8));
		cloudinary->Add(new wxHyperlinkCtrl(this, wxID_ANY, _("Register a Cloudinary account"),
			"https://cloudinary.com/users/register_free"), wxSizerFlags().Border(wxBOTTOM, 8));
		remember_cloudinary = new wxCheckBox(this, wxID_ANY,
			_("Store the API secret securely in the operating system credential store"));
		remember_cloudinary->SetValue(true);
		cloudinary->Add(remember_cloudinary, wxSizerFlags().Border(wxBOTTOM, 8));
		auto cloud_actions = new wxBoxSizer(wxHORIZONTAL);
		auto test_cloudinary = new wxButton(this, wxID_ANY, _("Test Cloudinary"));
		auto delete_cloudinary = new wxButton(this, wxID_ANY, _("Delete stored secret"));
		cloud_actions->Add(test_cloudinary);
		cloud_actions->Add(delete_cloudinary, wxSizerFlags().Border(wxLEFT));
		cloudinary->Add(cloud_actions);

		providers->Add(openai, wxSizerFlags(1).Expand().Border(wxALL, 12));
		providers->Add(new wxStaticLine(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
			wxLI_VERTICAL), wxSizerFlags().Expand().Border(wxTOP | wxBOTTOM, 12));
		providers->Add(cloudinary, wxSizerFlags(1).Expand().Border(wxALL, 12));
		main->Add(providers, wxSizerFlags(1).Expand());

		auto actions = new wxBoxSizer(wxHORIZONTAL);
		actions->AddStretchSpacer();
		actions->Add(new wxButton(this, wxID_CANCEL, _("Cancel")));
		actions->Add(new wxButton(this, wxID_OK, _("Save")), wxSizerFlags().Border(wxLEFT));
		main->Add(actions, wxSizerFlags().Expand().Border());

		SetSizerAndFit(main);
		SetMinSize(FromDIP(wxSize(820, 500)));
		CenterOnParent();
		test_openai->Bind(wxEVT_BUTTON, &AIConnectionDialog::OnTestOpenAI, this);
		delete_openai->Bind(wxEVT_BUTTON, &AIConnectionDialog::OnDeleteOpenAI, this);
		test_cloudinary->Bind(wxEVT_BUTTON, &AIConnectionDialog::OnTestCloudinary, this);
		delete_cloudinary->Bind(wxEVT_BUTTON, &AIConnectionDialog::OnDeleteCloudinary, this);
		Bind(wxEVT_BUTTON, &AIConnectionDialog::OnOK, this, wxID_OK);
		UpdateStatus();
	}
};

} // namespace

bool ShowAIConnectionDialog(wxWindow *parent, bool require_key) {
	AIConnectionDialog dialog(parent, require_key);
	return dialog.ShowModal() == wxID_OK && (!require_key || !ai::GetApiKey().empty());
}
