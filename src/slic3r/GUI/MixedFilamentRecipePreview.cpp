#include "MixedFilamentRecipePreview.hpp"

#include "MixedFilamentBadge.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>

namespace Slic3r::GUI {

MixedFilamentRecipePreview::MixedFilamentRecipePreview(wxWindow*                          parent,
                                                       const MixedFilamentLegacyRow&      filament,
                                                       const MixedFilamentDisplayContext& context,
                                                       bool                               has_error)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetCursor(wxCursor(wxCURSOR_HAND));
    SetMinSize(FromDIP(wxSize(100, 30)));
    SetMaxSize(FromDIP(wxSize(-1, 30)));
    SetFont(Label::Body_12);

    if (is_layer_gradient(filament)) {
        m_components     = mixed_gradient_components(filament, context.num_physical);
        m_gradient_stops = mixed_gradient_stops(filament, context.num_physical);
        for (int sample = 0; sample <= 64; ++sample)
            m_gradient_colors.emplace_back(mixed_gradient_display_color(filament, context, double(sample) / 64.0));
    } else {
        const auto definition = mixed_filament_definition_from_legacy_row(filament, context.num_physical);
        m_manual_pattern      = definition.recipe.kind == MixedFilamentRecipeKind::ManualPattern;
        if (m_manual_pattern && definition.recipe.manual_pattern && !definition.recipe.manual_pattern->groups.empty()) {
            for (const auto& component : definition.recipe.manual_pattern->groups.front())
                m_components.push_back(component.id);
        } else {
            m_components  = definition.recipe.blend.component_ids();
            m_percentages = definition.recipe.blend.component_percents();
        }
    }

    for (unsigned int id : m_components) {
        const wxColour color(id > 0 && id <= context.physical_colors.size() ? context.physical_colors[id - 1] : "#808080");
        m_colors.push_back(color.IsOk() ? color : wxColour("#808080"));
    }
    if (has_error)
        m_error_bitmap = ScalableBitmap(this, "error_icon_red_exclamation", 14).bmp();

    Bind(wxEVT_PAINT, &MixedFilamentRecipePreview::on_paint, this);
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& event) {
        m_hovered = true;
        Refresh();
        event.Skip();
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
        m_hovered = false;
        Refresh();
        event.Skip();
    });
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        Refresh();
        event.Skip();
    });
}

void MixedFilamentRecipePreview::draw_swatch(wxDC& dc, const wxRect& rect, const wxColour& color, const wxString& label)
{
    if (rect.width <= 0 || rect.height <= 0)
        return;
    wxDCClipper clip(dc, rect);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(color));
    dc.DrawRectangle(rect);

    const bool dark = StateColor::darkModeColorFor(*wxWHITE) != *wxWHITE;
    if ((dark && color.Red() < 45 && color.Green() < 45 && color.Blue() < 45) ||
        (!dark && color.Red() > 224 && color.Green() > 224 && color.Blue() > 224)) {
        dc.SetPen(wxPen(dark ? wxColour(130, 130, 128) : wxColour(207, 207, 207)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRectangle(rect);
    }

    dc.SetTextForeground(color.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);
    const wxSize text_size = dc.GetTextExtent(label);
    if (text_size.x <= rect.width)
        dc.DrawText(label, rect.x + (rect.width - text_size.x) / 2, rect.y + (rect.height - text_size.y) / 2);
}

void MixedFilamentRecipePreview::draw_mix(wxDC& dc, const wxRect& rect)
{
    const int count = static_cast<int>(m_components.size());
    if (count == 0 || m_percentages.size() != m_components.size())
        return;

    const int gap   = count == 1 ? 0 : std::min(FromDIP(5), std::max(0, (rect.width - count) / (count - 1)));
    const int width = rect.width - gap * (count - 1);
    if (width <= 0)
        return;

    // Retain the community card's proportional blocks, with room for small components' labels.
    const int minimum      = std::min({FromDIP(20), int(std::round(0.2 * width)), width / count});
    const int threshold    = 100 * minimum / width;
    const int remainder    = width - count * minimum;
    int       total_weight = 0;
    for (int percent : m_percentages)
        total_weight += std::max(0, percent - threshold);

    int used              = 0;
    int cumulative_weight = 0;
    for (int index = 0; index < count; ++index) {
        cumulative_weight += std::max(0, m_percentages[index] - threshold);
        const int      end   = index + 1 == count ?
                                   width :
                                   (index + 1) * minimum +
                                (total_weight > 0 ? int(std::round(double(remainder) * cumulative_weight / total_weight)) : 0);
        const wxString label = m_hovered ? wxString::Format("%d%%", m_percentages[index]) : wxString::Format("%u", m_components[index]);
        draw_swatch(dc, wxRect(rect.x + used + index * gap, rect.y, end - used, rect.height), m_colors[index], label);
        used = end;
    }
}

void MixedFilamentRecipePreview::draw_pattern(wxDC& dc, const wxRect& rect)
{
    const int   width = std::max(1, std::min(FromDIP(20), int(std::round(rect.width / 4.5))));
    wxDCClipper clip(dc, rect);
    for (size_t index = 0; index < m_components.size() && int(index) * width < rect.width; ++index)
        draw_swatch(dc, wxRect(rect.x + int(index) * width, rect.y, width, rect.height), m_colors[index],
                    wxString::Format("%u", m_components[index]));

    if (m_components.size() * size_t(width) > size_t(rect.width)) {
        std::unique_ptr<wxGraphicsContext> graphics(wxGraphicsContext::CreateFromUnknownDC(dc));
        if (graphics) {
            const int      fade       = std::min(FromDIP(30), rect.width / 3);
            const wxColour background = StateColor::darkModeColorFor(*wxWHITE);
            graphics->SetPen(*wxTRANSPARENT_PEN);
            graphics->SetBrush(graphics->CreateLinearGradientBrush(rect.GetRight() - fade, rect.y, rect.GetRight(), rect.y,
                                                                   wxColour(background.Red(), background.Green(), background.Blue(), 0),
                                                                   background));
            graphics->DrawRectangle(rect.GetRight() - fade, rect.y, fade + 1, rect.height);
        }
    }
}

void MixedFilamentRecipePreview::draw_gradient(wxDC& dc, const wxRect& rect)
{
    for (int x = 0; x < rect.width; ++x) {
        const double position = rect.width > 1 ? double(x) / (rect.width - 1) : 0.0;
        dc.SetPen(wxPen(interpolate_color(m_gradient_colors, position)));
        dc.DrawLine(rect.x + x, rect.y, rect.x + x, rect.y + rect.height);
    }

    const size_t count = m_components.size();
    if (count < 2 || m_gradient_stops.size() != 2 * count - 1)
        return;
    const int             padding = FromDIP(4);
    std::vector<wxString> labels;
    std::vector<int>      widths;
    std::vector<int>      left;
    for (size_t index = 0; index < count; ++index) {
        labels.push_back(wxString::Format("%u", m_components[index]));
        widths.push_back(dc.GetTextExtent(labels.back()).x);
        left.push_back(rect.x + int(std::round(m_gradient_stops[2 * index] * (rect.width - 1))) - widths.back() / 2);
    }
    if (std::accumulate(widths.begin(), widths.end(), 0) + int(count + 1) * padding > rect.width)
        return; // Keep the strip readable when a narrow sidebar cannot fit all labels.
    left.front() = std::max(rect.x + padding, left.front());
    for (size_t index = 1; index < count; ++index)
        left[index] = std::max(left[index], left[index - 1] + widths[index - 1] + padding);
    left.back() = std::min(left.back(), rect.GetRight() + 1 - padding - widths.back());
    for (size_t index = count - 1; index > 0; --index)
        left[index - 1] = std::min(left[index - 1], left[index] - padding - widths[index - 1]);

    for (size_t index = 0; index < count; ++index) {
        const double   position = double(left[index] + widths[index] / 2 - rect.x) / std::max(1, rect.width - 1);
        const wxColour color    = interpolate_color(m_gradient_colors, position);
        dc.SetTextForeground(color.GetLuminance() > 0.5 ? wxColour(50, 58, 61) : *wxWHITE);
        dc.DrawText(labels[index], left[index], rect.y + (rect.height - dc.GetTextExtent(labels[index]).y) / 2);
    }
}

void MixedFilamentRecipePreview::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
    dc.Clear();
    dc.SetFont(GetFont());
    const wxSize size    = GetClientSize();
    const int    padding = FromDIP(5);
    const int    height  = std::max(0, std::min(FromDIP(20), size.y - 2));
    wxRect       content(padding, (size.y - height) / 2, std::max(0, size.x - 2 * padding), height);
    if (m_error_bitmap.IsOk()) {
        dc.DrawBitmap(m_error_bitmap, content.x, (size.y - m_error_bitmap.GetHeight()) / 2, true);
        const int error_width = m_error_bitmap.GetWidth() + FromDIP(4);
        content.x += error_width;
        content.width = std::max(0, content.width - error_width);
    }
    if (content.width > 0 && content.height > 0) {
        if (!m_gradient_colors.empty())
            draw_gradient(dc, content);
        else if (m_manual_pattern)
            draw_pattern(dc, content);
        else
            draw_mix(dc, content);
    }

    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour(m_hovered ? "#009688" : "#DBDBDB"))));
    dc.DrawRectangle(GetClientRect());
}

} // namespace Slic3r::GUI
