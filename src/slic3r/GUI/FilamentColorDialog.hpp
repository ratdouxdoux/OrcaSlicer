#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"

#include <string>
#include <utility>
#include <vector>

class wxStaticBitmap;
class wxStaticText;
class wxColourData;

namespace Slic3r
{
namespace GUI
{

// Returns wxID_MORE when the user requests the Snapmaker family palette.
int ShowClassicFilamentColorDialog(wxWindow* parent, wxColourData& data);

class FilamentPaletteDialog : public DPIDialog
{
public:
    explicit FilamentPaletteDialog(wxWindow* parent);

protected:
    void PlaceNearFilamentPanel();
    void UpdateRoundedShape();
    void BindDragWindow(wxWindow* window);
    void on_dpi_changed(const wxRect& suggestedRect) override;

private:
    void    StartDrag(wxMouseEvent& event);
    void    DragDialog(wxMouseEvent& event);
    void    EndDrag(wxMouseEvent& event);
    bool    _dragPending{false};
    bool    _isDragging{false};
    wxPoint _dragStartMouse;
    wxPoint _dragStartPosition;
};

struct FilamentFamilyChoice
{
    FilamentColorInfo filament;
    std::string       presetName;
};

class FilamentFamilyDialog : public FilamentPaletteDialog
{
public:
    FilamentFamilyDialog(wxWindow* parent, const std::vector<FilamentFamilyChoice>& families, const std::string& currentFamily);
    size_t Selection() const { return _selection; }

private:
    size_t _selection{0};
};

/**
 * @brief Dialog for choosing a built-in filament color or a custom color.
 */
class FilamentColorDialog : public FilamentPaletteDialog
{
public:
    /**
     * @brief Creates the filament color dialog.
     */
    FilamentColorDialog(wxWindow* parent, const FilamentColorInfo& filament, const FilamentColor& currentColor, bool selectOnClick = false);

    /**
     * @brief Gets the selected color result.
     */
    const FilamentColor& Selection() const
    {
        return _selection;
    }

private:
    void BuildUi();
    void SelectFilamentColor(const FilamentColorItem& colorItem);
    void SelectCustomColor(const std::string& color);
    void UpdatePreview();
    void UpdateSwatchSelection();
    void OpenMoreColorDialog();

private:
    FilamentColorInfo _filament;
    std::string _languageCode;
    FilamentColor _selection;
    std::string _selectedSku;
    std::string _highlightSku;
    std::vector<std::pair<wxWindow*, std::string>> _swatchBySku;
    wxStaticBitmap* _previewBitmap { nullptr };
    wxStaticText* _nameLabel { nullptr };
    wxStaticText* _skuLabel { nullptr };
    bool                                           _selectOnClick{false};
};

} // namespace GUI
} // namespace Slic3r
