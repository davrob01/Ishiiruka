// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <wx/arrstr.h>
#include <wx/panel.h>

class DolphinSlider;
class wxCheckBox;
class wxChoice;
class wxRadioBox;
class wxSpinCtrl;
class wxStaticText;

class AudioConfigPane final : public wxPanel
{
public:
	AudioConfigPane(wxWindow* parent, wxWindowID id);

private:
	void InitializeGUI();
	void LoadGUIValues();
	void BindEvents();

	void PopulateBackendChoiceBox();
	void ToggleBackendSpecificControls(const std::string& backend);

	void OnDSPEngineRadioBoxChanged(wxCommandEvent&);
	void OnDPL2DecoderCheckBoxChanged(wxCommandEvent&);
	void OnVolumeSliderChanged(wxCommandEvent&);
	void OnAudioBackendChanged(wxCommandEvent&);
	void OnLatencySpinCtrlChanged(wxCommandEvent&);
	void OnTimeStretchingCheckBoxChanged(wxCommandEvent&);
	void OnRS_Hack_checkboxChanged(wxCommandEvent&);

	wxArrayString m_dsp_engine_strings;
	wxArrayString m_audio_backend_strings;

	wxRadioBox* m_dsp_engine_radiobox;
	wxCheckBox* m_dpl2_decoder_checkbox;
	DolphinSlider* m_volume_slider;
	wxStaticText* m_volume_text;
	wxChoice* m_audio_backend_choice;
	wxSpinCtrl* m_audio_latency_spinctrl;
	wxCheckBox* m_time_stretching_checkbox;
	wxCheckBox* m_RS_Hack_checkbox;
	wxStaticText* m_audio_latency_label;
};
