#ifndef slic3r_GUI_MixedFilamentExportDialog_hpp_
#define slic3r_GUI_MixedFilamentExportDialog_hpp_

#include "GUI_Utils.hpp"

#include <optional>
#include <string>
#include <vector>

class wxButton;
class wxCheckBox;
class wxCheckListBox;
class wxRadioButton;
class wxScrolledWindow;
class wxStaticText;

namespace Slic3r { namespace GUI {

class MixedFilamentExportDialog : public DPIDialog
{
public:
    explicit MixedFilamentExportDialog(wxWindow* parent);

    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    struct PhysicalFilament
    {
        unsigned int          slot = 0;
        std::string           name;
        std::string           color_hex;
        std::optional<double> td_mm;
    };

    void load_physical_filaments();
    void update_state_and_summary();
    void on_export(wxCommandEvent& event);

    std::vector<PhysicalFilament> selected_physical_filaments() const;

    std::vector<PhysicalFilament> m_physical_filaments;

    wxCheckListBox*   m_filament_list      = nullptr;
    wxRadioButton*    m_export_current     = nullptr;
    wxRadioButton*    m_generate_automatic = nullptr;
    wxCheckBox*       m_include_pairs      = nullptr;
    wxCheckBox*       m_include_triples    = nullptr;
    wxCheckBox*       m_include_quads      = nullptr;
    wxCheckBox*       m_include_kmks       = nullptr;
    wxScrolledWindow* m_scrolled_content   = nullptr;
    wxStaticText*     m_count_summary      = nullptr;
    wxButton*         m_export_button      = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_MixedFilamentExportDialog_hpp_
