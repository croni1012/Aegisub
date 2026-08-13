#include <wx/wx.h>
#include <wx/spinbutt.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <functional>

static wxString FormatValue(double v)
{
    v = std::round(v * 1000.0) / 1000.0;
    wxString s = wxString::Format("%.3f", v);

    while (s.Contains(".") && (s.EndsWith("0") || s.EndsWith("."))) {
        if (s.EndsWith(".")) {
            s.RemoveLast();
            break;
        }

        s.RemoveLast();
    }

    return s;
}

struct NumericSliderConfig
{
    double min;
    double max;
    double step;
    double minPreciseStep;
    double preciseStep;
    double maxPreciseStep;

    std::vector<double> presets;
};

static NumericSliderConfig GetConfig(const wxString& label)
{
    wxString l = label.Lower();

    if (l == "fs")
        return {
            0, 500, // min, max
            1, 0.1, 0.01, 0.001, // step, minPreciseStep, preciseStep, maxPreciseStep
            {8,12,16,20,24,28,32,36,40,45,50,55,60,65,70,80,90,100,120,140,180,200,250} // choices
        };

    if (l == "fscx" || l == "fscy")
        return {
            0, 1000, // min, max
            1, 0.1, 0.01, 0.001, // step, minPreciseStep, preciseStep, maxPreciseStep
            {0,10,25,50,75,90,100,105,110,115,125,150,200,250,500} // choices
        };

    if (l == "fsp")
        return {
            -100, 100, // min, max
            1, 0.1, 0.01, 0.001, // step, minPreciseStep, preciseStep, maxPreciseStep
            {-10,-5,-1,0,1,1.5,2,3,4,5,6,7,8,9,10,15,20,25} // choices
        };

    return {
        0, 100, // min, max
        1, 0.1, 0.01, 0.001, // step, minPreciseStep, preciseStep, maxPreciseStep
        {} // choices
    };
}

class NumericSliderCtrl : public wxPanel {
public:
    NumericSliderCtrl(
        wxWindow* parent, const wxString& label, double defaultValue, std::function<void(double)> onChange=nullptr)
        : wxPanel(parent), label(label), defaultValue(defaultValue), value(defaultValue), onChange(onChange)
    {
        cfg = GetConfig(label);
        sliderResolution = 100000;

        auto* main = new wxBoxSizer(wxVERTICAL);
        auto* row  = new wxBoxSizer(wxHORIZONTAL);

        labelText = new wxStaticText(this,wxID_ANY,label + ":");
        text = new wxTextCtrl(
            this,
            wxID_ANY,
            FormatValue(value),
            wxDefaultPosition,
            wxSize(70,22),
            wxTE_PROCESS_ENTER
        );

        spin = new wxSpinButton(this,wxID_ANY,wxDefaultPosition,wxSize(18,22));
        spin->SetRange(-100000,100000);

        row->Add(labelText,0,wxALIGN_CENTER_VERTICAL|wxRIGHT,6);
        row->Add(text,0,wxRIGHT,2);
        row->Add(spin,0);

        main->Add(row,0,wxEXPAND|wxBOTTOM,4);

        slider = new wxSlider(this,wxID_ANY,0,0,sliderResolution);
        main->Add(slider,0,wxEXPAND);

        SetSizer(main);
        UpdateSlider();

        slider->Bind(wxEVT_SLIDER,&NumericSliderCtrl::OnSlider,this);
        slider->Bind(wxEVT_SCROLL_THUMBTRACK,&NumericSliderCtrl::OnSlider,this);
        slider->Bind(wxEVT_MOUSEWHEEL,&NumericSliderCtrl::OnWheel,this);
        slider->Bind(wxEVT_RIGHT_UP,&NumericSliderCtrl::OnReset,this);

        text->Bind(wxEVT_TEXT_ENTER,&NumericSliderCtrl::OnTextCommit,this);
        text->Bind(wxEVT_KILL_FOCUS,&NumericSliderCtrl::OnTextKillFocus,this);
        text->Bind(wxEVT_LEFT_DCLICK,&NumericSliderCtrl::OnDropdown,this);
        text->Bind(wxEVT_MOUSEWHEEL,&NumericSliderCtrl::OnWheel,this);
        text->Bind(wxEVT_RIGHT_UP,&NumericSliderCtrl::OnReset,this);
        text->Bind(wxEVT_TEXT,&NumericSliderCtrl::OnTextLive,this);

        spin->Bind(wxEVT_SPIN_UP,&NumericSliderCtrl::OnSpinUp,this);
        spin->Bind(wxEVT_SPIN_DOWN,&NumericSliderCtrl::OnSpinDown,this);

        slider->SetToolTip("- " + _("Right click to reset") + "\n- " + _("Hold CTRL, SHIFT or both to be more precise while using mouse wheel"));
        text->SetToolTip("- " + _("Right click to reset") + "\n- " + _("Hold CTRL, SHIFT or both to be more precise while using mouse wheel") + "\n- " + _("Double click to see options"));
        spin->SetToolTip("- " + _("Right click to reset") + "\n- " + _("Hold CTRL, SHIFT or both to be more precise while using mouse wheel"));
    }

    double GetValue() const { return std::round(value * 1000.0) / 1000.0; }
private:
    wxString label;
    NumericSliderConfig cfg;
    wxStaticText* labelText;
    wxTextCtrl* text;
    wxSpinButton* spin;
    wxSlider* slider;
    int sliderResolution;
    double defaultValue;
    double value;

    std::function<void(double)> onChange;

    void Notify()
    {
        if (onChange)
            onChange(value);
    }

    double Clamp(double v)
    {
        return std::clamp(v, cfg.min, cfg.max);
    }

    double Snap(double v, double step)
    {
        return std::round(v / step) * step;
    }

    void SetValue(double v, bool notify = true)
    {
        v = std::round(v * 1000.0) / 1000.0;
        value = Clamp(v);

        text->ChangeValue(FormatValue(value));
        text->SetInsertionPoint(text->GetLastPosition());

        UpdateSlider();

        if (notify)
            Notify();
    }

    void UpdateSlider()
    {
        double t = (value - cfg.min) / (cfg.max - cfg.min);
        int pos = t * sliderResolution;

        slider->SetValue(std::clamp(pos,0,sliderResolution));
    }

    double GetStep(bool ctrl,bool shift)
    {
        if (ctrl && shift)
            return cfg.maxPreciseStep;

        if (ctrl)
            return cfg.preciseStep;

        if (shift)
            return cfg.minPreciseStep;

        return cfg.step;
    }

    void OnSlider(wxCommandEvent&)
    {
        double t = slider->GetValue()/(double)sliderResolution;
        double v = cfg.min + t*(cfg.max-cfg.min);

        v = Snap(v, cfg.step);
        SetValue(v);
    }

    void OnWheel(wxMouseEvent& e)
    {
        double step = GetStep(e.ControlDown(),e.ShiftDown());
        double v = value;

        if (e.GetWheelRotation() > 0)
            v += step;
        else
            v -= step;

        SetValue(v);
    }

    void OnSpinUp(wxSpinEvent&)
    {
        double step = GetStep(wxGetKeyState(WXK_CONTROL), wxGetKeyState(WXK_SHIFT));
        SetValue(value + step);
    }

    void OnSpinDown(wxSpinEvent&)
    {
        double step = GetStep(wxGetKeyState(WXK_CONTROL), wxGetKeyState(WXK_SHIFT));
        SetValue(value - step);
    }

    void OnReset(wxMouseEvent& e)
    {
        SetValue(defaultValue);
        e.Skip();
    }

    void OnTextLive(wxCommandEvent&)
    {
        double v;

        if (text->GetValue().ToDouble(&v)) {
            value = Clamp(v);
            UpdateSlider();
        }

        Notify();
    }

    void OnTextCommit(wxEvent&)
    {
        double v;

        if (text->GetValue().ToDouble(&v))
            SetValue(v);
        else {
            text->ChangeValue(FormatValue(value));
            text->SetInsertionPoint(text->GetLastPosition());
        }
    }

    void OnTextKillFocus(wxFocusEvent& e)
    {
        double v;

        if (text->GetValue().ToDouble(&v))
            SetValue(v);
        else {
            text->ChangeValue(FormatValue(value));
            text->SetInsertionPoint(text->GetLastPosition());
        }

        e.Skip();
    }

    void OnDropdown(wxMouseEvent&)
    {
        if (cfg.presets.empty())
            return;

        wxMenu menu;

        for (size_t i = 0; i < cfg.presets.size(); i++) {
            int id = 5000 + i;

            menu.AppendCheckItem(id, FormatValue(cfg.presets[i]));

            if (std::abs(cfg.presets[i]-value) < 0.0001)
                menu.Check(id,true);

            menu.Bind(wxEVT_MENU,[=, this](wxCommandEvent&)
            {
                SetValue(cfg.presets[i]);
            }, id);
        }

        PopupMenu(&menu);
    }
};
