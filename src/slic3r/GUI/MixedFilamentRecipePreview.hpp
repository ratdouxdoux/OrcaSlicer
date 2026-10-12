#pragma once

#include <wx/bitmap.h>
#include <wx/panel.h>

#include <vector>

class wxDC;

namespace Slic3r {
struct MixedFilamentLegacyRow;
struct MixedFilamentDisplayContext;

namespace GUI {

// Sidebar recipe rendering adapted from the community fork's FilamentCardMixed.
// Own the preview data so rebuilding the mixed-filament list cannot invalidate it.
class MixedFilamentRecipePreview : public wxPanel
{
public:
    MixedFilamentRecipePreview(wxWindow*                          parent,
                               const MixedFilamentLegacyRow&      filament,
                               const MixedFilamentDisplayContext& context,
                               bool                               has_error);

private:
    void on_paint(wxPaintEvent& event);
    void draw_mix(wxDC& dc, const wxRect& rect);
    void draw_pattern(wxDC& dc, const wxRect& rect);
    void draw_gradient(wxDC& dc, const wxRect& rect);
    void draw_swatch(wxDC& dc, const wxRect& rect, const wxColour& color, const wxString& label);

    std::vector<unsigned int> m_components;
    std::vector<int>          m_percentages;
    std::vector<wxColour>     m_colors;
    std::vector<wxColour>     m_gradient_colors;
    std::vector<float>        m_gradient_stops;
    wxBitmap                  m_error_bitmap;
    bool                      m_manual_pattern{false};
    bool                      m_hovered{false};
};

} // namespace GUI
} // namespace Slic3r
