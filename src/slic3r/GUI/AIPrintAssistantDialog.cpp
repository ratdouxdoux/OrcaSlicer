
#include "AIPrintAssistantDialog.hpp"
#include "AIModelTools.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <wx/base64.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/collpane.h>
#include <wx/dialog.h>
#include <wx/file.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/html/htmlwin.h>
#include <wx/html/htmlcell.h>
#include <wx/listbox.h>
#include <wx/mstream.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/timer.h>
#include <wx/weakref.h>
#include "libslic3r/AIPrintAssistant.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/AIPrintAssistantChat.hpp"
#include "slic3r/Utils/AIPrintAssistantCredentials.hpp"
#include "slic3r/Utils/AIChatMarkdown.hpp"
#include "slic3r/Utils/Http.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "ParamsPanel.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "Tab.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#ifdef _MSW_DARK_MODE
#include "dark_mode.hpp"
#endif

namespace Slic3r::GUI {
namespace {
namespace AI = AIPrintAssistant;
using json   = nlohmann::json;
wxString    from_utf8(const std::string& text) { return wxString::FromUTF8(text.data(), text.size()); }
std::string to_utf8(const wxString& text) { return std::string(text.ToUTF8().data()); }

wxColour assistant_color(const wxColour& light) { return StateColor::darkModeColorFor(light); }

wxString model_action_label(const std::string& action)
{
    static const std::map<std::string, wxString> labels = {{"add_text", _L("Add text")},
                                                           {"edit_text", _L("Edit text")},
                                                           {"add_primitive", _L("Add shape")},
                                                           {"create_mix", _L("Create mixed filament")},
                                                           {"match_color", _L("Match a color")},
                                                           {"paint_surface", _L("Paint a surface")},
                                                           {"assign_filament", _L("Assign filament")},
                                                           {"transform", _L("Transform model")},
                                                           {"drop_to_bed", _L("Drop to bed")},
                                                           {"duplicate", _L("Duplicate")},
                                                           {"delete", _L("Delete")},
                                                           {"rename", _L("Rename")},
                                                           {"set_part_type", _L("Change part type")},
                                                           {"split_parts", _L("Split into parts")},
                                                           {"boolean", _L("Combine shapes")},
                                                           {"simplify", _L("Simplify mesh")},
                                                           {"cut", _L("Cut model")},
                                                           {"open_editor", _L("Open model editor")},
                                                           {"native_action", _L("Edit in workspace")}};
    const auto                                   found  = labels.find(action);
    return found == labels.end() ? from_utf8(action) : found->second;
}

wxString readable_model_value(const json& value)
{
    if (value.is_string()) {
        auto text = from_utf8(value.get<std::string>());
        text.Replace("_", " ");
        return text;
    }
    if (value.is_boolean())
        return value.get<bool>() ? _L("Yes") : _L("No");
    if (value.is_array()) {
        wxString result;
        for (const auto& item : value) {
            if (!result.empty())
                result += ", ";
            result += readable_model_value(item);
        }
        return result;
    }
    return from_utf8(value.dump());
}

Button* assistant_button(wxWindow* parent, const wxString& text, bool primary = false, const wxString& icon = {})
{
    auto* button = new Button(parent, text, icon, 0, 16);
    button->SetFont(Label::Body_13);
    button->SetPaddingSize(parent->FromDIP(wxSize(10, 5)));
    button->SetMinSize(parent->FromDIP(wxSize(28, 30)));
    button->SetCornerRadius(parent->FromDIP(5));
    button->SetBorderWidth(1);
    button->SetBackgroundColor(StateColor(std::make_pair(wxColour("#F0F0F1"), int(StateColor::Disabled)),
                                          std::make_pair(wxColour(primary ? "#26A69A" : "#F1F1F1"), int(StateColor::Hovered)),
                                          std::make_pair(wxColour(primary ? "#009688" : "#FFFFFF"), int(StateColor::Normal))));
    button->SetBorderColor(StateColor(std::make_pair(wxColour("#009688"), int(StateColor::Focused)),
                                      std::make_pair(wxColour(primary ? "#009688" : "#DBDBDB"), int(StateColor::Normal))));
    button->SetTextColor(StateColor(std::make_pair(wxColour("#6B6B6B"), int(StateColor::Disabled)),
                                    std::make_pair(wxColour(primary ? "#FEFEFE" : "#262E30"), int(StateColor::Normal))));
    return button;
}

Button* activity_button(wxWindow* parent, const wxString& text)
{
    auto* button = assistant_button(parent, text);
    button->SetCenter(false);
    button->SetFont(Label::Body_12);
    button->SetBorderColor(StateColor(std::make_pair(wxColour("#009688"), int(StateColor::Focused)),
                                      std::make_pair(wxColour("#FFFFFF"), int(StateColor::Normal))));
    button->SetTextColor(StateColor(wxColour("#6B6B6A")));
    return button;
}

wxString disclosure_label(const wxString& text, bool expanded) { return wxString(expanded ? L"\u25BE  " : L"\u25B8  ") + text; }

struct RequestResult
{
    std::atomic<bool> cancelled{false};
    std::mutex        mutex;
    bool              done = false;
    std::string       body, error;
};

class AIPrintAssistantPanel final : public wxPanel
{
public:
    AIPrintAssistantPanel(wxWindow* parent, Plater* plater, std::function<void()> close);
    ~AIPrintAssistantPanel() override { cancel_request(); }
    void update_theme();
    void suspend()
    {
        if (m_result) {
            cancel_request();
            add_message(_L("Stopped"), _L("The request was cancelled."));
        }
    }

private:
    Plater*                     m_plater;
    wxTimer                     m_timer;
    wxScrolledWindow*           m_body       = nullptr;
    wxBoxSizer*                 m_messages   = nullptr;
    ComboBox*                   m_scope      = nullptr;
    wxTextCtrl*                 m_prompt     = nullptr;
    Label*                      m_status     = nullptr;
    Label*                      m_connection = nullptr;
    Button *                    m_send = nullptr, *m_stop = nullptr, *m_settings = nullptr, *m_new = nullptr;
    Button *                    m_add_images = nullptr, *m_remove_image = nullptr, *m_apply = nullptr, *m_revert = nullptr;
    Button *                    m_dismiss = nullptr, *m_native_apply = nullptr;
    wxPanel*                    m_preview_bar   = nullptr;
    Label*                      m_preview_label = nullptr;
    wxPanel*                    m_welcome       = nullptr;
    StaticBox*                  m_composer      = nullptr;
    wxPanel*                    m_attachments   = nullptr;
    wxListBox*                  m_image_list    = nullptr;
    wxStaticBitmap*             m_image_preview = nullptr;
    std::vector<AI::ImageInput> m_images, m_turn_images;
    std::vector<wxBitmap>       m_thumbnails;
    AI::Provider                m_provider = AI::DEFAULT_PROVIDER;
    std::string                 m_key, m_model = AI::provider_info(AI::DEFAULT_PROVIDER).default_model;
    json                        m_history = json::array();
    struct TextBlock
    {
        wxHtmlWindow* label;
        wxString      text;
        wxString      rendered;
        int           width = -1;
    };
    std::vector<TextBlock> m_texts;
    wxHtmlWindow*          m_stream_text = nullptr;
    wxPanel*               m_stream_card = nullptr;
    struct Activity
    {
        wxPanel *panel = nullptr, *content = nullptr;
        Button*  toggle = nullptr;
        Label*   status = nullptr;
        wxString title;
        int      tools = 0, errors = 0;
        bool     has_updates = false;
    };
    std::shared_ptr<Activity>         m_activity;
    wxString                          m_tool_summary;
    std::shared_ptr<RequestResult>    m_result;
    Http::Ptr                         m_request;
    std::unique_ptr<AI::ChatStream>   m_stream;
    size_t                            m_consumed = 0;
    int                               m_rounds   = 0;
    std::optional<AI::Context>        m_pending;
    std::optional<AI::ModelOperation> m_model_operation;
    std::unique_ptr<AI::LivePreview>  m_preview;
    json                              m_last_tool_result;
    wxPoint                           m_resize_start;
    int                               m_resize_width   = 0;
    bool                              m_layout_pending = false;
    int                               m_body_width     = -1;
    struct ThemeBinding
    {
        wxWeakRef<wxWindow> window;
        wxColour            background, foreground;
    };
    std::vector<ThemeBinding> m_theme_bindings;

    void style_window(wxWindow* window, const wxColour& background = wxColour("#FFFFFF"), const wxColour& foreground = wxColour("#262E30"));
    Label*        add_label(wxWindow* parent, const wxString& text, const wxFont& font, bool muted = false);
    wxPanel*      add_card(const wxString& title, bool user = false);
    wxHtmlWindow* add_text(wxWindow* parent, const wxString& text);
    void          add_message(const wxString& title, const wxString& text);
    void          set_text(wxHtmlWindow* label, const wxString& text);
    void          layout_chat(bool scroll = true);
    void          begin_activity();
    void          update_activity(const wxString& status);
    void          refresh_activity(const std::shared_ptr<Activity>& activity);
    void          finish_activity(const wxString& status, bool hide_empty = false);
    void          render_stream();
    void          add_tool_card(const AI::ChatToolCall& call, const json& result, bool error);
    void          settings();
    void          new_chat();
    void          set_busy(bool busy);
    void          show_error(const std::exception& error);
    void          request_proposal();
    void          start_request();
    void          poll_request();
    void          cancel_request(const wxString& status = _L("Stopped"));
    AI::Scope     selected_scope() const;
    Tab*          target_tab() const;
    AI::Context   current_context(ModelObject** object = nullptr, const std::string& object_id = {}, bool inspect = false);
    void          invalidate_proposal();
    void          add_images();
    void          remove_image();
    void          preview_image();
    void          display_proposal(AI::Proposal proposal);
    void          begin_preview();
    void          record_preview();
    void          update_preview();
    void          accept_preview();
    bool          restore_preview(bool notify = true);
    void          display_model_operation(AI::ModelOperation operation);
    void          apply_model_operation();
    void          validate_preset_candidate(const DynamicPrintConfig& config);
    void          commit_preset(DynamicPrintConfig config);
    void          commit(ModelObject& object, DynamicPrintConfig config, const wxString& description);
    void          refresh_config_ui(ModelObject& object);
    void          export_context();
    void          import_proposal();
};

void AIPrintAssistantPanel::style_window(wxWindow* window, const wxColour& background, const wxColour& foreground)
{
    m_theme_bindings.push_back({window, background, foreground});
    window->SetBackgroundColour(assistant_color(background));
    window->SetForegroundColour(assistant_color(foreground));
#ifdef _MSW_DARK_MODE
    if (dynamic_cast<wxScrollHelper*>(window) || dynamic_cast<wxTextCtrl*>(window) || dynamic_cast<wxListBox*>(window))
        NppDarkMode::SetDarkExplorerTheme(window->GetHWND());
#endif
}

Label* AIPrintAssistantPanel::add_label(wxWindow* parent, const wxString& text, const wxFont& font, bool muted)
{
    auto*      label      = new Label(parent, font, text);
    const auto background = parent->GetBackgroundColour();
    style_window(label, wxGetApp().dark_mode() ? StateColor::lightModeColorFor(background) : background,
                 wxColour(muted ? "#6B6B6A" : "#262E30"));
    return label;
}

void AIPrintAssistantPanel::update_theme()
{
    m_theme_bindings.erase(std::remove_if(m_theme_bindings.begin(), m_theme_bindings.end(),
                                          [](const ThemeBinding& item) { return !item.window; }),
                           m_theme_bindings.end());
    for (const auto& item : m_theme_bindings) {
        item.window->SetBackgroundColour(assistant_color(item.background));
        item.window->SetForegroundColour(assistant_color(item.foreground));
#ifdef _MSW_DARK_MODE
        if (dynamic_cast<wxScrollHelper*>(item.window.get()) || dynamic_cast<wxTextCtrl*>(item.window.get()) ||
            dynamic_cast<wxListBox*>(item.window.get()))
            NppDarkMode::SetDarkExplorerTheme(item.window->GetHWND());
#endif
        item.window->Refresh();
    }
    for (auto& block : m_texts)
        block.width = -1;
    layout_chat(false);
    Refresh();
}

AIPrintAssistantPanel::AIPrintAssistantPanel(wxWindow* parent, Plater* plater, std::function<void()> close)
    : wxPanel(parent), m_plater(plater), m_timer(this)
{
    auto& config = *wxGetApp().app_config;
    for (const auto provider : AI::PROVIDERS)
        if (config.get("ai_print_assistant", "provider") == AI::provider_id(provider))
            m_provider = provider;
    m_model = config.get("ai_print_assistant", std::string("model_") + AI::provider_id(m_provider));
    if (m_model.empty())
        m_model = AI::provider_info(m_provider).default_model;
    std::string credential_error;
    if (config.get("ai_print_assistant", std::string("key_saved_") + AI::provider_id(m_provider)) == "1") {
        try {
            m_key = AI::load_api_key(m_provider, data_dir());
        } catch (const std::exception& error) {
            credential_error = error.what();
        }
    }
    SetFont(Label::Body_14);
    SetMinSize(FromDIP(wxSize(400, -1)));
    style_window(this);
    auto* outer = new wxBoxSizer(wxHORIZONTAL);
    auto* grip  = new wxPanel(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(4, -1)));
    style_window(grip, wxColour("#EEEEEE"));
    grip->SetCursor(wxCursor(wxCURSOR_SIZEWE));
    outer->Add(grip, 0, wxEXPAND);
    auto* root = new wxBoxSizer(wxVERTICAL);
    outer->Add(root, 1, wxEXPAND);
    SetSizer(outer);
    grip->Bind(wxEVT_LEFT_DOWN, [this, grip](wxMouseEvent& event) {
        m_resize_start = grip->ClientToScreen(event.GetPosition());
        m_resize_width = GetSize().x;
        grip->CaptureMouse();
    });
    grip->Bind(wxEVT_MOTION, [this, grip](wxMouseEvent& event) {
        if (grip->HasCapture() && event.Dragging()) {
            int width = std::clamp(m_resize_width + m_resize_start.x - grip->ClientToScreen(event.GetPosition()).x, FromDIP(340),
                                   std::max(FromDIP(340), GetParent()->GetClientSize().x * 2 / 3));
            SetMinSize(wxSize(width, -1));
            GetParent()->Layout();
            layout_chat(false);
        }
    });
    grip->Bind(wxEVT_LEFT_UP, [grip](wxMouseEvent&) {
        if (grip->HasCapture())
            grip->ReleaseMouse();
    });
    grip->Bind(wxEVT_MOUSE_CAPTURE_LOST, [](wxMouseCaptureLostEvent&) {});

    auto* header = new wxBoxSizer(wxHORIZONTAL);
    header->Add(add_label(this, _L("Assistant"), Label::Head_16), 1, wxALIGN_CENTER_VERTICAL);
    m_new = assistant_button(this, _L("New chat"));
    m_new->SetToolTip(_L("Start a new conversation"));
    m_settings = assistant_button(this, wxEmptyString, false, "settings");
    m_settings->SetName(_L("Assistant settings"));
    m_settings->SetToolTip(_L("Assistant settings"));
    auto* hide = assistant_button(this, wxString::FromUTF8("\xC3\x97"));
    hide->SetName(_L("Hide assistant"));
    hide->SetToolTip(_L("Hide assistant"));
    header->Add(m_new, 0, wxRIGHT, FromDIP(6));
    header->Add(m_settings, 0, wxRIGHT, FromDIP(6));
    header->Add(hide);
    root->Add(header, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    m_connection = add_label(this,
                             from_utf8(AI::provider_info(m_provider).name) +
                                 (m_key.empty() ? _L("  /  Set up in Settings") : _L("  /  Ready")),
                             Label::Body_11, true);
    m_connection->SetToolTip(from_utf8(m_model));
    root->Add(m_connection, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    root->AddSpacer(FromDIP(12));
    auto* separator = new wxPanel(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(-1, 1)));
    style_window(separator, wxColour("#EEEEEE"));
    root->Add(separator, 0, wxEXPAND);

    m_body = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE);
    style_window(m_body);
    m_body->SetScrollRate(0, FromDIP(12));
    m_messages = new wxBoxSizer(wxVERTICAL);
    m_body->SetSizer(m_messages);
    root->Add(m_body, 1, wxEXPAND);

    // A single persistent action bar owns the complete preview, not each step.
    m_preview_bar = new wxPanel(this);
    style_window(m_preview_bar, wxColour("#F8F8F8"));
    auto* preview_layout = new wxBoxSizer(wxVERTICAL);
    m_preview_bar->SetSizer(preview_layout);
    m_preview_label = add_label(m_preview_bar, _L("Live preview"), Label::Body_12);
    preview_layout->Add(m_preview_label, 0, wxEXPAND | wxALL, FromDIP(8));
    auto* preview_actions = new wxBoxSizer(wxHORIZONTAL);
    m_apply               = assistant_button(m_preview_bar, _L("Apply changes"), true);
    m_dismiss             = assistant_button(m_preview_bar, _L("Dismiss"));
    m_revert              = assistant_button(m_preview_bar, _L("Revert changes"));
    preview_actions->Add(m_apply, 0, wxRIGHT, FromDIP(6));
    preview_actions->Add(m_dismiss, 0, wxRIGHT, FromDIP(6));
    preview_actions->Add(m_revert);
    preview_layout->Add(preview_actions, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
    root->Add(m_preview_bar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
    m_preview_bar->Hide();
    m_apply->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept_preview(); });
    m_dismiss->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { restore_preview(); });
    m_revert->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { restore_preview(); });

    // One bordered composer contains the attachment tray, text entry and actions.
    m_composer = new StaticBox(this);
    m_composer->SetCornerRadius(FromDIP(8));
    m_composer->SetBackgroundColor(StateColor(wxColour("#FFFFFF")));
    m_composer->SetBorderColor(StateColor(std::make_pair(wxColour("#009688"), int(StateColor::Focused)),
                                          std::make_pair(wxColour("#DBDBDB"), int(StateColor::Normal))));
    auto* input = new wxBoxSizer(wxVERTICAL);
    m_composer->SetSizer(input);
    root->Add(m_composer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_attachments = new wxPanel(m_composer);
    style_window(m_attachments);
    auto* attachments = new wxBoxSizer(wxHORIZONTAL);
    m_image_list      = new wxListBox(m_attachments, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(100, 54)), 0, nullptr, wxBORDER_NONE);
    m_image_list->SetFont(Label::Body_12);
    style_window(m_image_list);
    m_image_preview = new wxStaticBitmap(m_attachments, wxID_ANY, wxNullBitmap, wxDefaultPosition, FromDIP(wxSize(80, 54)));
    m_remove_image  = assistant_button(m_attachments, _L("Remove"));
    attachments->Add(m_image_preview, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
    attachments->Add(m_image_list, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(6));
    attachments->Add(m_remove_image, 0, wxALIGN_CENTER_VERTICAL);
    m_attachments->SetSizer(attachments);
    m_attachments->Hide();
    input->Add(m_attachments, 0, wxEXPAND | wxALL, FromDIP(10));

    // Use a normal label: Windows' native multiline editor does not paint cue banners.
    auto* prompt_label = add_label(m_composer, _L("Ask about your print"), Label::Body_12, true);
    input->Add(prompt_label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    m_prompt = new wxTextCtrl(m_composer, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(-1, 60)),
                              wxTE_MULTILINE | wxBORDER_NONE);
    m_prompt->SetFont(Label::Body_14);
    style_window(m_prompt);
    m_prompt->SetName(_L("Message the print assistant"));
    input->Add(m_prompt, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    auto* compose = new wxBoxSizer(wxHORIZONTAL);
    m_add_images  = assistant_button(m_composer, _L("+ Images"));
    m_add_images->SetToolTip(_L("Attach PNG or JPEG images"));
    compose->Add(m_add_images, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(6));
    m_scope = new ComboBox(m_composer, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(130, 30)), 0, nullptr, wxCB_READONLY);
    m_scope->SetFont(Label::Body_12);
    m_scope->SetCornerRadius(FromDIP(4));
    m_scope->SetMinSize(FromDIP(wxSize(100, 30)));
    for (const auto& label : {_L("Object"), _L("Global process"), _L("Global filament"), _L("Global printer")})
        m_scope->Append(label);
    m_scope->SetSelection(0);
    m_scope->SetToolTip(_L("Choose Object for individual overrides, or a Global scope to edit the shared preset."));
    style_window(m_scope->GetTextCtrl());
    compose->Add(m_scope, 1, wxRIGHT | wxALIGN_CENTER_VERTICAL, FromDIP(6));
    m_send = assistant_button(m_composer, _L("Send"), true);
    m_stop = assistant_button(m_composer, _L("Stop"));
    compose->Add(m_send, 0, wxALIGN_CENTER_VERTICAL);
    compose->Add(m_stop, 0, wxALIGN_CENTER_VERTICAL);
    input->Add(compose, 0, wxEXPAND | wxALL, FromDIP(8));
    m_status = add_label(this, credential_error.empty() ? _L("Enter to send / Shift+Enter for a new line") : from_utf8(credential_error),
                         Label::Body_11, true);
    m_status->SetWindowStyleFlag(m_status->GetWindowStyleFlag() | LB_AUTO_WRAP);
    root->Add(m_status, 0, wxEXPAND | wxALL, FromDIP(12));

    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { request_proposal(); });
    m_prompt->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if ((event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER) && !event.ShiftDown()) {
            if (!m_result)
                request_proposal();
        } else
            event.Skip();
    });
    auto focus_border = [this](wxFocusEvent& event) {
        m_composer->SetBorderColor(StateColor(wxColour(event.GetEventType() == wxEVT_SET_FOCUS ? "#009688" : "#DBDBDB")));
        event.Skip();
    };
    m_prompt->Bind(wxEVT_SET_FOCUS, focus_border);
    m_prompt->Bind(wxEVT_KILL_FOCUS, focus_border);
    m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { suspend(); });
    m_settings->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { settings(); });
    m_new->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { new_chat(); });
    hide->Bind(wxEVT_BUTTON, [close](wxCommandEvent&) { close(); });
    m_scope->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { invalidate_proposal(); });
    m_add_images->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add_images(); });
    m_remove_image->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { remove_image(); });
    m_image_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { preview_image(); });
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { poll_request(); });
    auto schedule_layout = [this](wxSizeEvent& event) {
        event.Skip();
        if (!m_layout_pending) {
            m_layout_pending = true;
            CallAfter([this] {
                m_layout_pending = false;
                layout_chat(false);
            });
        }
    };
    Bind(wxEVT_SIZE, schedule_layout);
    m_body->Bind(wxEVT_SIZE, [this, schedule_layout](wxSizeEvent& event) {
        // FitInside emits size events even when the viewport width is unchanged.
        // Only a width change can invalidate wrapping; avoid an endless layout queue.
        if (m_body_width != m_body->GetClientSize().x) {
            m_body_width = m_body->GetClientSize().x;
            schedule_layout(event);
        } else
            event.Skip();
    });
    Bind(wxEVT_DPI_CHANGED, [this](wxDPIChangedEvent& event) {
        // Native controls rescale themselves; refresh custom controls and cached HTML heights.
        std::function<void(wxWindow*)> rescale = [&](wxWindow* window) {
            if (auto* button = dynamic_cast<Button*>(window)) {
                button->Rescale();
                button->SetPaddingSize(FromDIP(wxSize(10, 5)));
                button->SetMinSize(FromDIP(wxSize(28, 30)));
                button->SetCornerRadius(FromDIP(5));
            } else if (auto* combo = dynamic_cast<ComboBox*>(window))
                combo->Rescale();
            for (auto* child : window->GetChildren())
                rescale(child);
        };
        rescale(this);
        m_body->SetScrollRate(0, FromDIP(12));
        m_prompt->SetMinSize(FromDIP(wxSize(-1, 60)));
        m_composer->SetCornerRadius(FromDIP(8));
        CallAfter([this] { update_theme(); });
        event.Skip();
    });
    new_chat();
}

wxPanel* AIPrintAssistantPanel::add_card(const wxString& title, bool user)
{
    auto* card = new wxPanel(m_body);
    style_window(card, wxColour(user ? "#F8F8F8" : "#FFFFFF"));
    card->SetSizer(new wxBoxSizer(wxVERTICAL));
    auto* heading = add_label(card, title, Label::Head_13);
    card->GetSizer()->Add(heading, 0, wxEXPAND | wxALL, FromDIP(10));
    m_messages->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    return card;
}

wxHtmlWindow* AIPrintAssistantPanel::add_text(wxWindow* parent, const wxString& text)
{
    auto* label = new wxHtmlWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(1, 20), wxHW_SCROLLBAR_NEVER);
    // wxHtmlWindow owns a scroll helper even with its scrollbars disabled. It
    // consumes wheel events instead of scrolling the surrounding transcript.
    label->Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
        wxMouseEvent forwarded(event);
        forwarded.SetEventObject(m_body);
        forwarded.SetId(m_body->GetId());
        m_body->GetEventHandler()->ProcessEvent(forwarded);
    });
    label->Bind(wxEVT_HTML_LINK_CLICKED, [](wxHtmlLinkEvent& event) {
        const auto url = to_utf8(event.GetLinkInfo().GetHref());
        if (AI::chat_markdown_web_link(url))
            wxLaunchDefaultBrowser(from_utf8(url));
    });
    label->SetBorders(0);
    label->SetBackgroundColour(parent->GetBackgroundColour());
    const int fs      = Label::Body_14.GetPointSize();
    const int sizes[] = {fs, fs, fs, fs + 1, fs + 2, fs + 3, fs + 4};
    label->SetFonts(Label::Body_14.GetFaceName(), "Consolas", sizes);
    parent->GetSizer()->Add(label, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));
    m_texts.push_back({label, text});
    return label;
}
void AIPrintAssistantPanel::set_text(wxHtmlWindow* label, const wxString& text)
{
    for (auto& block : m_texts)
        if (block.label == label) {
            block.text = text;
            break;
        }
}
void AIPrintAssistantPanel::add_message(const wxString& title, const wxString& text)
{
    add_text(add_card(title), text);
    layout_chat();
}
void AIPrintAssistantPanel::layout_chat(bool scroll)
{
    if (!m_body)
        return;
    int scroll_x, scroll_y, unit_x, unit_y;
    m_body->GetViewStart(&scroll_x, &scroll_y);
    m_body->GetScrollPixelsPerUnit(&unit_x, &unit_y);
    const bool follow = scroll && scroll_y * unit_y + m_body->GetClientSize().y >= m_body->GetVirtualSize().y - FromDIP(36);
    Layout();
    m_body->Layout();
    for (auto& block : m_texts) {
        // Collapsed activity may contain large tool histories. Render only when
        // opened, after its parents have their actual width.
        if (!block.label->IsShownOnScreen())
            continue;
        const int width = std::max(FromDIP(180), block.label->GetParent()->GetClientSize().x - FromDIP(20));
        if (block.width == width && block.rendered == block.text)
            continue;
        block.label->SetSize(wxSize(width, block.label->GetSize().y));
        const auto            background = block.label->GetParent()->GetBackgroundColour();
        const auto            foreground = assistant_color(wxColour("#262E30"));
        AI::ChatMarkdownStyle markdown;
        markdown.background = to_utf8(background.GetAsString(wxC2S_HTML_SYNTAX));
        markdown.foreground = to_utf8(foreground.GetAsString(wxC2S_HTML_SYNTAX));
        markdown.inset      = to_utf8(assistant_color(wxColour("#F8F8F8")).GetAsString(wxC2S_HTML_SYNTAX));
        // The theme's accent is a button fill, too dark for link text on dark backgrounds.
        markdown.link           = background.GetLuminance() < 0.5 ? "#5ACABB" : "#00796B";
        markdown.compact_tables = width < FromDIP(420);
        const wxFont code_font(wxFontInfo(Label::Body_14.GetPointSize()).FaceName("Consolas"));
        wxCoord      code_width = 0;
        block.label->GetTextExtent("0000000000", &code_width, nullptr, nullptr, nullptr, &code_font);
        markdown.code_columns = unsigned(std::max(16, (width - FromDIP(24)) * 10 / std::max(1, code_width)));
        block.label->SetPage(from_utf8(AI::chat_markdown_html(to_utf8(block.text), markdown)));
        // SetPage resets the HTML window background; restore it after parsing too.
        block.label->SetBackgroundColour(background);
        block.label->SetForegroundColour(foreground);
        block.label->SetMinSize(wxSize(1, block.label->GetInternalRepresentation()->GetHeight() + FromDIP(4)));
        block.width    = width;
        block.rendered = block.text;
    }
    m_body->Layout();
    m_body->FitInside();
    Layout();
    if (follow)
        m_body->Scroll(-1, m_body->GetVirtualSize().y);
}

void AIPrintAssistantPanel::begin_activity()
{
    if (m_activity)
        return;
    auto activity   = std::make_shared<Activity>();
    activity->panel = new wxPanel(m_body);
    style_window(activity->panel);
    activity->panel->SetSizer(new wxBoxSizer(wxVERTICAL));
    activity->toggle  = activity_button(activity->panel, wxEmptyString);
    activity->content = new wxPanel(activity->panel);
    style_window(activity->content);
    activity->content->SetSizer(new wxBoxSizer(wxVERTICAL));
    activity->status = add_label(activity->content, _L("Thinking..."), Label::Body_12, true);
    activity->content->GetSizer()->Add(activity->status, 0, wxEXPAND | wxALL, FromDIP(10));
    activity->content->Hide();
    activity->panel->GetSizer()->Add(activity->toggle, 0, wxEXPAND);
    activity->panel->GetSizer()->Add(activity->content, 0, wxEXPAND | wxLEFT, FromDIP(8));
    m_messages->Add(activity->panel, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    activity->toggle->Bind(wxEVT_BUTTON, [this, activity](wxCommandEvent&) {
        activity->content->Show(!activity->content->IsShown());
        refresh_activity(activity);
        layout_chat(false);
    });
    m_activity = std::move(activity);
    update_activity(_L("Thinking..."));
}

void AIPrintAssistantPanel::refresh_activity(const std::shared_ptr<Activity>& activity)
{
    auto title = activity->title;
    if (activity->tools == 1)
        title += _L(" · 1 tool");
    else if (activity->tools)
        title += wxString::Format(_L(" · %d tools"), activity->tools);
    if (activity->errors)
        title += wxString::Format(_L(" · %d failed"), activity->errors);
    activity->toggle->SetLabel(disclosure_label(title, activity->content->IsShown()));
    activity->toggle->SetToolTip(activity->content->IsShown() ? _L("Hide activity") : _L("Show working status and tool calls"));
}

void AIPrintAssistantPanel::update_activity(const wxString& status)
{
    if (!m_activity)
        return;
    m_activity->title = status;
    m_activity->status->SetLabel(status);
    refresh_activity(m_activity);
}

void AIPrintAssistantPanel::finish_activity(const wxString& status, bool hide_empty)
{
    if (!m_activity)
        return;
    update_activity(status);
    m_activity->status->Show(!m_activity->tools && !m_activity->has_updates);
    if (hide_empty && !m_activity->tools && !m_activity->has_updates)
        m_activity->panel->Hide();
    m_activity.reset();
    m_stream_card = nullptr;
    m_stream_text = nullptr;
}

void AIPrintAssistantPanel::render_stream()
{
    const auto text        = from_utf8(m_stream->text());
    const bool using_tools = !m_stream->active_tools().empty();
    update_activity(using_tools          ? _L("Using tools...") :
                    !text.empty()        ? _L("Responding...") :
                    m_stream->thinking() ? _L("Thinking...") :
                                           _L("Working..."));
    if (!text.empty() && !m_stream_card) {
        m_stream_card = add_card(_L("Assistant"));
        m_stream_text = add_text(m_stream_card, text);
    } else if (m_stream_text)
        set_text(m_stream_text, text);
    if (using_tools && m_stream_card && m_stream_card->GetParent() == m_body) {
        // A streamed preamble becomes an intermediate update once tools arrive.
        // Keep only the final answer outside the activity disclosure.
        m_messages->Detach(m_stream_card);
        m_stream_card->Reparent(m_activity->content);
        m_activity->content->GetSizer()->Add(m_stream_card, 0, wxEXPAND | wxTOP, FromDIP(4));
        auto* heading = static_cast<Label*>(m_stream_card->GetChildren().GetFirst()->GetData());
        heading->SetLabel(_L("Update"));
        heading->SetFont(Label::Body_12);
        m_activity->has_updates = true;
    }
}

void AIPrintAssistantPanel::add_tool_card(const AI::ChatToolCall& call, const json& result, bool error)
{
    auto title = call.name == "get_print_context" ? _L("Read print settings") :
                 call.name == "get_slice_context" ? _L("Inspected slice result") :
                 call.name == "read_gcode"        ? _L("Read slice G-code") :
                 call.name == "search_settings"   ? _L("Searched available settings") :
                 call.name == "get_model_context" ? _L("Inspected models and colors") :
                 call.name == "list_model_tools"  ? _L("Read available model tools") :
                 call.name == "propose_model_operation" && call.arguments.is_object() && call.arguments.contains("operation") &&
                         call.arguments["operation"].is_string() ?
                                                   model_action_label(call.arguments["operation"].get<std::string>()) :
                                                   _L("Update print settings");
    if (error)
        title += _L(" — failed");
    begin_activity();
    ++m_activity->tools;
    m_activity->errors += error ? 1 : 0;
    auto* card = new wxPanel(m_activity->content);
    style_window(card);
    card->SetSizer(new wxBoxSizer(wxVERTICAL));
    m_activity->content->GetSizer()->Add(card, 0, wxEXPAND | wxBOTTOM, FromDIP(2));
    auto* details = activity_button(card, disclosure_label(title, false));
    auto* content = new wxPanel(card);
    style_window(content);
    content->SetSizer(new wxBoxSizer(wxVERTICAL));
    const auto text = json({{"arguments", call.arguments}, {"result", result}}).dump(2);
    // Allocate the raw JSON control only if this particular tool is expanded.
    const auto summary = error ? from_utf8(result.value("message", "")) : m_tool_summary;
    content->Hide();
    card->GetSizer()->Add(details, 0, wxEXPAND);
    card->GetSizer()->Add(content, 0, wxEXPAND | wxLEFT | wxBOTTOM, FromDIP(10));
    details->Bind(wxEVT_BUTTON, [this, details, content, title, summary, text, name = call.name](wxCommandEvent&) {
        if (content->GetChildren().empty()) {
            if (!summary.empty())
                add_text(content, summary);
            auto* raw = new wxTextCtrl(content, wxID_ANY, from_utf8(name + "\n" + text), wxDefaultPosition, FromDIP(wxSize(-1, 170)),
                                       wxTE_MULTILINE | wxTE_READONLY | wxBORDER_NONE);
            raw->SetFont(Label::Body_12);
            style_window(raw, wxColour("#F8F8F8"));
            content->GetSizer()->Add(raw, 0, wxEXPAND | wxALL, FromDIP(10));
        }
        content->Show(!content->IsShown());
        details->SetLabel(disclosure_label(title, content->IsShown()));
        layout_chat(false);
    });
    refresh_activity(m_activity);
    layout_chat();
}

void AIPrintAssistantPanel::settings()
{
    wxDialog dialog(this, wxID_ANY, _L("Assistant settings"), wxDefaultPosition, FromDIP(wxSize(490, 430)),
                    wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    dialog.SetFont(Label::Body_14);
    style_window(&dialog);
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(add_label(&dialog, _L("Connect your assistant"), Label::Head_18), 0, wxALL, FromDIP(16));
    auto* grid = new wxFlexGridSizer(2, FromDIP(10), FromDIP(10));
    grid->AddGrowableCol(1);
    auto label = [&](const wxString& text) { grid->Add(add_label(&dialog, text, Label::Body_13), 0, wxALIGN_CENTER_VERTICAL); };
    label(_L("Provider"));
    auto* provider = new ComboBox(&dialog, wxID_ANY, wxEmptyString, wxDefaultPosition, FromDIP(wxSize(-1, 32)), 0, nullptr, wxCB_READONLY);
    provider->SetCornerRadius(FromDIP(4));
    for (size_t i = 0; i < AI::PROVIDERS.size(); ++i) {
        provider->Append(from_utf8(AI::provider_info(AI::PROVIDERS[i]).name));
        if (AI::PROVIDERS[i] == m_provider)
            provider->SetSelection(static_cast<int>(i));
    }
    grid->Add(provider, 1, wxEXPAND);
    label(_L("API key"));
    auto* key_input = new TextInput(&dialog, from_utf8(m_key), {}, {}, wxDefaultPosition, FromDIP(wxSize(-1, 32)), wxTE_PASSWORD);
    key_input->SetCornerRadius(FromDIP(4));
    auto* key = key_input->GetTextCtrl();
    key->SetHint(_L("Paste your API key"));
    grid->Add(key_input, 1, wxEXPAND);
    label(_L("Model"));
    auto* model_input = new TextInput(&dialog, from_utf8(m_model), {}, {}, wxDefaultPosition, FromDIP(wxSize(-1, 32)));
    model_input->SetCornerRadius(FromDIP(4));
    auto* model = model_input->GetTextCtrl();
    grid->Add(model_input, 1, wxEXPAND);
    label(_L("API endpoint"));
    auto* endpoint = add_label(&dialog, from_utf8(AI::provider_info(m_provider).endpoint), Label::Body_12, true);
    endpoint->Wrap(FromDIP(320));
    grid->Add(endpoint, 1, wxEXPAND);
    root->Add(grid, 0, wxEXPAND | wxALL, FromDIP(16));
    auto* disclosure =
        add_label(&dialog,
                  _L("Messages, images, model summaries, lettering, filament recipes and public settings (including custom G-code) go to "
                     "your provider. OpenRouter forwards them to "
                     "its model provider. API usage is billed by the provider. Keys are saved per provider in your operating system's "
                     "credential store. Clear the key and Save to remove it."),
                  Label::Body_12, true);
    disclosure->Wrap(FromDIP(440));
    root->Add(disclosure, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    auto* advanced = assistant_button(&dialog, _L("Developer tools"));
    auto* tools    = new wxPanel(&dialog);
    style_window(tools);
    auto* tool_buttons  = new wxBoxSizer(wxHORIZONTAL);
    auto* export_button = assistant_button(tools, _L("Export context"));
    auto* import_button = assistant_button(tools, _L("Load proposal"));
    tool_buttons->Add(export_button, 0, wxRIGHT, FromDIP(8));
    tool_buttons->Add(import_button);
    tools->SetSizer(tool_buttons);
    tools->Hide();
    root->Add(advanced, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    root->Add(tools, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
    advanced->Bind(wxEVT_BUTTON, [&dialog, tools](wxCommandEvent&) {
        tools->Show(!tools->IsShown());
        dialog.Layout();
        dialog.Fit();
    });
    export_button->Bind(wxEVT_BUTTON, [this, &dialog](wxCommandEvent&) {
        dialog.EndModal(wxID_CANCEL);
        CallAfter([this] { export_context(); });
    });
    import_button->Bind(wxEVT_BUTTON, [this, &dialog](wxCommandEvent&) {
        dialog.EndModal(wxID_CANCEL);
        CallAfter([this] { import_proposal(); });
    });
    root->AddStretchSpacer();
    auto* actions = new wxBoxSizer(wxHORIZONTAL);
    auto* cancel  = assistant_button(&dialog, _L("Cancel"));
    auto* save    = assistant_button(&dialog, _L("Save"), true);
    cancel->SetId(wxID_CANCEL);
    save->SetId(wxID_OK);
    actions->AddStretchSpacer();
    actions->Add(cancel, 0, wxRIGHT, FromDIP(8));
    actions->Add(save);
    root->Add(actions, 0, wxEXPAND | wxALL, FromDIP(16));
    cancel->Bind(wxEVT_BUTTON, [&dialog](wxCommandEvent&) { dialog.EndModal(wxID_CANCEL); });
    save->Bind(wxEVT_BUTTON, [this, &dialog, provider, key, model](wxCommandEvent&) {
        try {
            const auto selected       = AI::PROVIDERS.at(provider->GetSelection());
            const auto selected_model = to_utf8(model->GetValue());
            AI::make_chat_request(selected, selected_model, json::array({AI::chat_user_message(selected, "validate")}));
            if (!m_history.empty() && (selected != m_provider || selected_model != m_model)) {
                new_chat();
                if (m_preview && m_preview->pending())
                    return;
            }
            const auto api_key = to_utf8(key->GetValue().Strip(wxString::both));
            AI::save_api_key(selected, data_dir(), api_key);
            auto&             config = *wxGetApp().app_config;
            const std::string id     = AI::provider_id(selected);
            config.set("ai_print_assistant", "provider", id);
            config.set("ai_print_assistant", "model_" + id, selected_model);
            config.set("ai_print_assistant", "key_saved_" + id, std::string(api_key.empty() ? "0" : "1"));
            config.save();
            key->ChangeValue(from_utf8(api_key));
            dialog.EndModal(wxID_OK);
        } catch (const std::exception& error) {
            wxMessageBox(from_utf8(error.what()), _L("Assistant settings"), wxOK | wxICON_ERROR, &dialog);
        }
    });
    dialog.SetSizer(root);
    dialog.SetMinSize(FromDIP(wxSize(490, 430)));
    dialog.Fit();
    wxGetApp().UpdateDlgDarkUI(&dialog);
    dialog.CentreOnScreen();
    std::map<AI::Provider, std::pair<wxString, wxString>> drafts;
    auto                                                  displayed_provider = m_provider;
    provider->Bind(wxEVT_COMBOBOX, [provider, key, model, endpoint, &dialog, &drafts, &displayed_provider](wxCommandEvent&) {
        drafts[displayed_provider] = {key->GetValue(), model->GetValue()};
        displayed_provider         = AI::PROVIDERS.at(provider->GetSelection());
        const auto info            = AI::provider_info(displayed_provider);
        if (!drafts.count(displayed_provider)) {
            auto&             config = *wxGetApp().app_config;
            const std::string id     = AI::provider_id(displayed_provider);
            std::string       saved_key;
            if (config.get("ai_print_assistant", "key_saved_" + id) == "1") {
                try {
                    saved_key = AI::load_api_key(displayed_provider, data_dir());
                } catch (const std::exception& error) {
                    wxMessageBox(from_utf8(error.what()), _L("Assistant settings"), wxOK | wxICON_ERROR, &dialog);
                }
            }
            const auto saved_model     = config.get("ai_print_assistant", "model_" + id);
            drafts[displayed_provider] = {from_utf8(saved_key), from_utf8(saved_model.empty() ? info.default_model : saved_model)};
        }
        key->ChangeValue(drafts[displayed_provider].first);
        model->ChangeValue(drafts[displayed_provider].second);
        endpoint->SetLabel(from_utf8(info.endpoint));
        endpoint->Wrap(dialog.FromDIP(320));
        dialog.Layout();
        dialog.Fit();
    });
    if (dialog.ShowModal() == wxID_OK) {
        const auto selected = AI::PROVIDERS.at(provider->GetSelection());
        m_provider          = selected;
        m_key               = to_utf8(key->GetValue());
        m_model             = to_utf8(model->GetValue());
        m_connection->SetLabel(from_utf8(std::string(AI::provider_info(m_provider).name)) +
                               (m_key.empty() ? _L("  /  Set up in Settings") : _L("  /  Ready")));
        m_connection->SetToolTip(from_utf8(m_model));
        m_status->SetLabel(_L("Enter to send / Shift+Enter for a new line"));
        m_prompt->SetFocus();
    }
}

void AIPrintAssistantPanel::new_chat()
{
    cancel_request();
    if (m_preview && m_preview->pending() && !restore_preview(false))
        return;
    m_preview.reset();
    m_pending.reset();
    invalidate_proposal();
    m_native_apply = nullptr;
    update_preview();
    m_stream_text = nullptr;
    m_stream_card = nullptr;
    m_activity.reset();
    m_tool_summary.clear();
    m_texts.clear();
    m_messages->Clear(true);
    m_history = json::array();
    m_images.clear();
    m_turn_images.clear();
    m_thumbnails.clear();
    m_image_list->Clear();
    m_attachments->Hide();
    m_prompt->Clear();
    m_welcome = add_card(_L("Let's work on your print"));
    static_cast<Label*>(m_welcome->GetChildren().GetFirst()->GetData())->SetFont(Label::Head_18);
    add_text(m_welcome, _L("Describe what you want to make or improve, or attach a photo. I can edit models, add text, mix colors and "
                           "adjust print settings."));
    auto* examples = new wxBoxSizer(wxVERTICAL);
    for (const auto& suggestion :
         {_L("Add hello to the top of this model"), _L("Help with a failed print"), _L("Mix a color for this model")}) {
        auto* button = assistant_button(m_welcome, suggestion);
        examples->Add(button, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
        button->Bind(wxEVT_BUTTON, [this, suggestion](wxCommandEvent&) {
            m_prompt->ChangeValue(suggestion);
            m_prompt->SetInsertionPointEnd();
            m_prompt->SetFocus();
        });
    }
    m_welcome->GetSizer()->Add(examples, 0, wxEXPAND | wxALL, FromDIP(10));
    m_status->SetLabel(_L("Enter to send / Shift+Enter for a new line"));
    update_theme();
    set_busy(false);
}
void AIPrintAssistantPanel::invalidate_proposal()
{
    m_model_operation.reset();
    if (m_native_apply)
        m_native_apply->Disable();
}

void AIPrintAssistantPanel::set_busy(bool busy)
{
    m_send->Show(!busy);
    m_stop->Show(busy);
    m_scope->Enable(!busy);
    m_settings->Enable(!busy);
    m_add_images->Enable(!busy && m_images.size() < AI::MAX_IMAGES);
    m_remove_image->Enable(!busy && m_image_list->GetSelection() != wxNOT_FOUND);
    m_prompt->Enable(!busy);
    m_new->Enable(!busy);
    m_apply->Enable(!busy && m_preview && m_preview->pending() && m_preview->steps() > 0);
    m_dismiss->Enable(m_preview && m_preview->pending() && m_preview->steps() > 0);
    m_revert->Enable(!busy && m_preview && !m_preview->pending());
    if (m_native_apply)
        m_native_apply->Enable(!busy && m_model_operation.has_value());
    m_composer->Layout();
    Layout();
}
void AIPrintAssistantPanel::show_error(const std::exception& error)
{
    add_message(_L("Something needs attention"), from_utf8(error.what()));
    m_status->SetLabel(_L("You can adjust your message and try again."));
}
void AIPrintAssistantPanel::cancel_request(const wxString& status)
{
    m_timer.Stop();
    if (m_result)
        m_result->cancelled.store(true);
    m_request.reset();
    m_result.reset();
    m_stream.reset();
    finish_activity(status);
    set_busy(false);
}
void AIPrintAssistantPanel::request_proposal()
{
    try {
        auto prompt = to_utf8(m_prompt->GetValue());
        if (prompt.empty() && !m_images.empty())
            prompt = "What can you tell me about these images?";
        if (prompt.empty())
            return;
        if (prompt.size() > 8000)
            throw std::runtime_error("Please keep each message under 8000 UTF-8 bytes.");
        if (m_key.empty()) {
            settings();
            if (m_key.empty())
                return;
        }
        if (m_key.size() > 4096 || m_key.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error("Check the API key in Settings.");
        if (m_history.size() >= 450)
            throw std::runtime_error("This conversation is full. Start a new chat to continue.");
        if (m_preview && m_preview->pending())
            m_preview->verify(ai_model_context(*m_plater, false).id);
        invalidate_proposal();
        m_turn_images = m_images;
        m_pending.reset();
        try {
            m_pending = current_context(nullptr, {}, true);
        } catch (const std::exception&) { /* General chat still works without supported print settings. */
        }
        if (m_welcome)
            m_welcome->Hide();
        auto* card = add_card(_L("You"), true);
        add_text(card, from_utf8(prompt));
        for (const auto& thumbnail : m_thumbnails)
            card->GetSizer()->Add(new wxStaticBitmap(card, wxID_ANY, thumbnail), 0, wxLEFT | wxBOTTOM, FromDIP(10));
        m_history.push_back(
            AI::chat_turn_message(m_provider, prompt, m_images, m_pending ? &*m_pending : nullptr, ai_plate_context(*m_plater)));
        m_prompt->Clear();
        m_images.clear();
        m_thumbnails.clear();
        m_image_list->Clear();
        m_attachments->Hide();
        m_rounds = 0;
        begin_activity();
        start_request();
    } catch (const std::exception& error) {
        cancel_request(_L("Interrupted"));
        show_error(error);
    }
}
void AIPrintAssistantPanel::start_request()
{
    if (++m_rounds > 32)
        throw std::runtime_error("The assistant reached this message's work limit. The preview is still available: send Continue to "
                                 "finish, Apply to keep it, or Dismiss to remove it.");
    const auto body = AI::make_chat_request(m_provider, m_model, m_history);
    m_stream        = std::make_unique<AI::ChatStream>(m_provider, m_key);
    m_consumed      = 0;
    m_stream_card   = nullptr;
    m_stream_text   = nullptr;
    update_activity(_L("Thinking..."));
    m_result     = std::make_shared<RequestResult>();
    auto result  = m_result;
    auto request = Http::post_external(AI::provider_info(m_provider).endpoint);
    if (AI::uses_chat_completions(m_provider))
        request.header("Authorization", "Bearer " + m_key);
    else
        request.header("anthropic-version", "2023-06-01").header("x-api-key", m_key);
    m_request = request.header("Content-Type", "application/json")
                    .timeout_connect(15)
                    .timeout_max(180)
                    .size_limit(2 * 1024 * 1024)
                    .set_post_body(body)
                    .on_progress([result](Http::Progress progress, bool& cancel) {
                        cancel = result->cancelled.load();
                        if (!cancel) {
                            std::lock_guard<std::mutex> lock(result->mutex);
                            if (!result->done)
                                result->body = progress.buffer;
                        }
                    })
                    .on_complete([result](std::string body, unsigned) {
                        std::lock_guard<std::mutex> lock(result->mutex);
                        result->body = std::move(body);
                        result->done = true;
                    })
                    .on_error(
                        [result, provider = m_provider, api_key = m_key](std::string body, std::string transport_error, unsigned status) {
                            std::lock_guard<std::mutex> lock(result->mutex);
                            result->error = AI::provider_error(provider, status, body, api_key, transport_error);
                            result->done  = true;
                        })
                    .perform();
    set_busy(true);
    m_status->SetLabel(_L("Working..."));
    layout_chat();
    m_timer.Start(100);
}
void AIPrintAssistantPanel::poll_request()
{
    if (!m_result)
        return;
    try {
        std::string delta, error;
        bool        done;
        {
            std::lock_guard<std::mutex> lock(m_result->mutex);
            done  = m_result->done;
            error = m_result->error;
            if (m_result->body.size() > m_consumed) {
                delta      = m_result->body.substr(m_consumed);
                m_consumed = m_result->body.size();
            }
        }
        if (!error.empty())
            throw std::runtime_error(error);
        if (!delta.empty()) {
            m_stream->feed(delta);
            render_stream();
            layout_chat();
        }
        if (!done)
            return;
        m_stream->finish();
        const auto calls    = m_stream->tool_calls();
        const auto response = m_stream->assistant_message();
        render_stream();
        if (calls.empty() && m_stream->text().empty())
            add_message(_L("Assistant"), _L("No answer was returned."));
        m_timer.Stop();
        m_result.reset();
        m_request.reset();
        m_stream.reset();
        m_history.push_back(response);
        for (const auto& call : calls) {
            m_tool_summary.clear();
            json result;
            bool failed = false;
            try {
                if (m_preview && m_preview->pending())
                    m_preview->verify(ai_model_context(*m_plater, false).id);
                if (call.name == "get_model_context" || call.name == "list_model_tools") {
                    if (!call.arguments.is_object() || !call.arguments.empty())
                        throw std::runtime_error("This tool takes no arguments.");
                    result = call.name == "get_model_context" ? ai_model_context(*m_plater).data : ai_model_tools(*m_plater);
                } else if (call.name == "get_slice_context" || call.name == "read_gcode") {
                    result = ai_read_slice_tool(*m_plater, call.name, call.arguments);
                } else if (call.name == "propose_model_operation") {
                    display_model_operation(AI::parse_model_operation(call.arguments));
                    result = m_last_tool_result;
                } else {
                    if (call.name == "propose_settings") {
                        auto proposal = AI::parse_proposal(call.arguments.dump());
                        if (proposal.outcome != "propose_changes")
                            throw std::runtime_error("Use normal chat for advice or questions; this tool requires proposed changes.");
                        display_proposal(std::move(proposal));
                        result = m_last_tool_result;
                    } else {
                        m_pending = current_context(nullptr, AI::read_tool_object_id(call), true);
                        result    = AI::run_read_tool(*m_pending, call);
                    }
                }
            } catch (const std::exception& error) {
                failed = true;
                result = {{"message", error.what()}};
            }
            add_tool_card(call, result, failed);
            m_history.push_back(AI::chat_tool_result(m_provider, call, result, failed));
        }
        if (!calls.empty())
            start_request();
        else {
            finish_activity(_L("Activity"), true);
            set_busy(false);
            m_status->SetLabel(m_preview && m_preview->pending() ? _L("Preview ready. Apply keeps it; Dismiss removes it.") : _L("Ready"));
            layout_chat();
            m_prompt->SetFocus();
        }
    } catch (const std::exception& error) {
        cancel_request(_L("Interrupted"));
        show_error(error);
    }
}

void AIPrintAssistantPanel::begin_preview()
{
    const auto id = ai_model_context(*m_plater, false).id;
    if (m_preview && m_preview->pending()) {
        m_preview->verify(id);
        return;
    }
    AI::PreviewCheckpoint before;
    before.state.model         = m_plater->model();
    auto& bundle               = *wxGetApp().preset_bundle;
    before.state.mixes         = bundle.mixed_filaments;
    before.state.model_changed = before.state.mixes_changed = true;
    const auto selected                                     = m_plater->get_selection().get_object_idx();
    if (selected >= 0 && size_t(selected) < m_plater->model().objects.size())
        before.state.result = {{"object_id", std::to_string(m_plater->model().objects[selected]->id().id)}};
    before.presets = {bundle.prints.get_edited_preset().config, bundle.filaments.get_edited_preset().config,
                      bundle.printers.get_edited_preset().config};
    auto preview   = std::make_unique<AI::LivePreview>(std::move(before), id);
    m_plater->take_snapshot("AI assistant changes");
    m_preview = std::move(preview);
    update_preview();
}

void AIPrintAssistantPanel::record_preview()
{
    m_preview->record(ai_model_context(*m_plater, false).id);
    m_last_tool_result = {{"status", "preview_updated"},
                          {"visible_in_slicer", true},
                          {"committed", false},
                          {"preview_steps", m_preview->steps()},
                          {"next", "Continue all remaining requested steps using fresh context. Do not wait for Apply."}};
    update_preview();
    m_status->SetLabel(_L("Preview updated. Apply keeps it; Dismiss removes it."));
}

void AIPrintAssistantPanel::update_preview()
{
    const bool pending = m_preview && m_preview->pending();
    m_preview_bar->Show(m_preview && m_preview->steps() > 0);
    m_apply->Show(pending);
    m_dismiss->Show(pending);
    m_revert->Show(m_preview && !pending);
    m_preview_label->SetLabel(
        m_preview ? wxString::Format(pending ? _L("Live preview (%d)") : _L("Applied (%d)"), int(m_preview->steps())) : _L("Live preview"));
    m_preview_bar->Layout();
    set_busy(bool(m_result));
}

void AIPrintAssistantPanel::accept_preview()
{
    try {
        if (!m_preview || m_result)
            return;
        m_preview->accept(ai_model_context(*m_plater, false).id);
        update_preview();
        m_status->SetLabel(_L("Changes applied."));
        add_message(_L("Changes applied"), _L("Kept all preview changes. Slice to inspect the print result."));
        m_history.push_back(AI::chat_user_message(m_provider, "[Application event] The user applied the complete live preview. "
                                                              "All previewed model, color and setting edits are accepted."));
    } catch (const std::exception& error) {
        show_error(error);
    }
}

bool AIPrintAssistantPanel::restore_preview(bool notify)
{
    try {
        if (!m_preview)
            return true;
        cancel_request();
        if (m_preview->steps() == 0) {
            m_preview.reset();
            update_preview();
            return true;
        }
        auto                                    before          = m_preview->before(ai_model_context(*m_plater, false).id);
        const bool                              pending         = m_preview->pending();
        auto&                                   bundle          = *wxGetApp().preset_bundle;
        const std::array<DynamicPrintConfig, 3> current         = {bundle.prints.get_edited_preset().config,
                                                                   bundle.filaments.get_edited_preset().config,
                                                                   bundle.printers.get_edited_preset().config};
        auto                                    restore_presets = [&](const std::array<DynamicPrintConfig, 3>& values) {
            const std::array<PresetCollection*, 3> collections = {&bundle.prints, &bundle.filaments, &bundle.printers};
            const std::array<Preset::Type, 3>      types = {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER};
            for (size_t i = 0; i < collections.size(); ++i) {
                if (collections[i]->get_edited_preset().config == values[i])
                    continue;
                collections[i]->get_edited_preset().config = values[i];
                auto* tab                                  = wxGetApp().get_tab(types[i]);
                tab->reload_config();
                tab->update_dirty();
            }
        };
        Plater::SuppressSnapshots suppress(m_plater);
        try {
            restore_presets(before.presets);
            ai_commit_model_edit(*m_plater, std::move(before.state), "Dismiss assistant preview");
        } catch (...) {
            restore_presets(current);
            m_plater->on_config_change(bundle.full_config());
            throw;
        }
        m_preview.reset();
        invalidate_proposal();
        update_preview();
        m_status->SetLabel(pending ? _L("Preview dismissed.") : _L("Changes reverted."));
        if (notify) {
            add_message(pending ? _L("Preview dismissed") : _L("Changes reverted"),
                        _L("Restored the models, colors and settings from before the preview."));
            m_history.push_back(AI::chat_user_message(m_provider, "[Application event] The entire assistant preview was removed. "
                                                                  "Models, mixing recipes and settings were restored."));
        }
        return true;
    } catch (const std::exception& error) {
        show_error(error);
        return false;
    }
}

void AIPrintAssistantPanel::display_proposal(AI::Proposal proposal)
{
    if (m_preview && m_preview->pending())
        m_preview->verify(ai_model_context(*m_plater, false).id);
    ModelObject* object  = nullptr;
    const auto   context = current_context(&object, proposal.object_id);
    if (proposal.context_id != context.id || proposal.object_id != context.public_context.at("object_id"))
        throw std::runtime_error("These settings changed. Read get_print_context again before continuing.");
    if (proposal.outcome != "propose_changes") {
        m_last_tool_result = {{"status", "no_changes"}};
        m_tool_summary     = from_utf8(proposal.explanation);
        add_message(_L("Assistant"), from_utf8(proposal.explanation));
        return;
    }
    AI::Transaction transaction;
    auto            candidate = AI::prepare_apply(context, proposal, transaction);
    if (target_tab())
        validate_preset_candidate(candidate);
    begin_preview();
    {
        Plater::SuppressSnapshots suppress(m_plater);
        if (context.scope == AI::Scope::Object)
            commit(*object, std::move(candidate), _L("Preview AI print settings"));
        else
            commit_preset(std::move(candidate));
    }
    record_preview();
    invalidate_proposal();
    m_tool_summary = from_utf8(proposal.explanation);
    for (const auto& key : transaction.changed_keys) {
        const auto* def = print_config_def.get(key);
        m_tool_summary += "\n\n" + from_utf8(def->label + "\n" + AI::setting_value(context.effective, key).dump() + "  ->  " +
                                             AI::setting_value(proposal.changes, key).dump());
    }
    if (context.scope != AI::Scope::Object)
        m_tool_summary += "\n\n" + _L("This preview affects the edited preset and objects that use it.");
    layout_chat();
}

void AIPrintAssistantPanel::display_model_operation(AI::ModelOperation operation)
{
    if (m_preview && m_preview->pending())
        m_preview->verify(ai_model_context(*m_plater, false).id);
    if (operation.context_id != ai_model_context(*m_plater).id)
        throw std::runtime_error("The model, selection or filaments changed. Read get_model_context again before proposing an edit.");
    std::unique_ptr<AI::PreparedModelEdit> prepared;
    if (operation.operation != "open_editor" && operation.operation != "native_action") {
        wxBusyCursor busy;
        prepared = std::make_unique<AI::PreparedModelEdit>(ai_prepare_model_edit(*m_plater, operation));
        if (!operation.object_id.empty())
            prepared->result["object_id"] = operation.object_id;
    }
    if (!prepared && m_preview && m_preview->pending() && m_preview->steps() > 0)
        throw std::runtime_error("This tool requires interaction in the native workspace. Complete the automatic preview first; "
                                 "the user can Apply or Dismiss it before opening a native editor or action.");
    invalidate_proposal();
    wxString description;
    for (const auto* object : m_plater->model().objects)
        if (std::to_string(object->id().id) == operation.object_id) {
            description += from_utf8(object->name);
            for (const auto* volume : object->volumes)
                if (std::to_string(volume->id().id) == operation.volume_id)
                    description += " / " + from_utf8(volume->name);
            description += "\n\n";
        }
    auto review_parameters = operation.parameters;
    if (operation.operation == "add_text" || operation.operation == "edit_text") {
        review_parameters["mode"] = operation.parameters.value("mode", "raised");
        review_parameters["bold"] = operation.parameters.value("bold", true);
    }
    for (auto it = review_parameters.begin(); it != review_parameters.end(); ++it) {
        if (it.key() == "instance_id" || it.key() == "other_volume_id")
            continue;
        auto label = from_utf8(it.key());
        label.Replace("_mm", " (mm)");
        label.Replace("_percent", " (%)");
        label.Replace("_degrees", " (degrees)");
        label.Replace("_", " ");
        if (!label.empty())
            label[0] = wxToupper(label[0]);
        // Keep user-provided lettering and names exactly as entered.
        const auto value = (it.key() == "text" || it.key() == "name") ? from_utf8(it.value().get<std::string>()) :
                                                                        readable_model_value(it.value());
        description += label + ": " + value + "\n";
    }
    if (prepared && prepared->mixes_changed) {
        wxString    recipe = _L("Mixing recipe:");
        const auto& result = prepared->result;
        for (size_t i = 0; i < result["components"].size(); ++i)
            recipe += "\n" +
                      wxString::Format(_L("Filament %d: %d%%"), result["components"][i].get<int>(), result["percentages"][i].get<int>());
        recipe += "\n" + _L("Approximate preview color:") + " " + from_utf8(result.value("preview_color", ""));
        description += "\n" + recipe;
    }
    if (prepared) {
        begin_preview();
        const auto details = prepared->result;
        {
            Plater::SuppressSnapshots suppress(m_plater);
            ai_commit_model_edit(*m_plater, std::move(*prepared), "Preview assistant model changes");
        }
        record_preview();
        m_last_tool_result["operation_result"] = details;
        m_tool_summary                         = description + "\n\n" + _L("Visible in the live preview.");
    } else {
        // Interactive handoffs remain visible; the user must be able to act
        // without discovering a button buried inside the tool history.
        auto* card = add_card(model_action_label(operation.operation));
        add_text(card, description);
        add_text(card, _L("This step needs interaction in the workspace."));
        m_native_apply = assistant_button(card, operation.operation == "open_editor" ? _L("Open editor") : _L("Run workspace action"), true);
        auto* dismiss = assistant_button(card, _L("Dismiss"));
        auto* actions = new wxBoxSizer(wxHORIZONTAL);
        actions->Add(m_native_apply, 0, wxRIGHT, FromDIP(6));
        actions->Add(dismiss);
        card->GetSizer()->Add(actions, 0, wxALL, FromDIP(10));
        const auto* button = m_native_apply;
        m_native_apply->Bind(wxEVT_BUTTON, [this, button](wxCommandEvent&) {
            if (m_native_apply == button && m_model_operation)
                apply_model_operation();
        });
        dismiss->Bind(wxEVT_BUTTON, [this, button, dismiss](wxCommandEvent&) {
            if (m_native_apply == button)
                invalidate_proposal();
            dismiss->Disable();
        });
        m_model_operation  = std::move(operation);
        m_last_tool_result = {{"status", "requires_user_interaction"}, {"committed", false}};
        m_tool_summary     = _L("This step needs interaction in the workspace.");
    }
    set_busy(bool(m_result));
    layout_chat();
}

void AIPrintAssistantPanel::apply_model_operation()
{
    try {
        if (!m_model_operation)
            return;
        if (m_preview && m_preview->pending() && m_preview->steps() > 0)
            throw std::runtime_error("Apply or Dismiss the live preview before using a native workspace tool.");
        if (ai_model_context(*m_plater).id != m_model_operation->context_id)
            throw std::runtime_error("The model, selection or filaments changed. Ask for a fresh workspace action.");
        const auto result = ai_run_native_model_tool(*m_plater, *m_model_operation);
        m_preview.reset();
        update_preview();
        invalidate_proposal();
        add_message(_L("Workspace action"), result.value("status", "") == "cancelled" ?
                                                _L("Cancelled.") :
                                                _L("Continue in the workspace. Native dialogs or background work may still be active."));
        m_history.push_back(AI::chat_user_message(m_provider, "[Application event] " + result.dump()));
        layout_chat();
    } catch (const std::exception& error) {
        invalidate_proposal();
        show_error(error);
    }
}

AI::Scope AIPrintAssistantPanel::selected_scope() const
{
    switch (m_scope->GetSelection()) {
    case 1: return AI::Scope::Process;
    case 2: return AI::Scope::Filament;
    case 3: return AI::Scope::Printer;
    default: return AI::Scope::Object;
    }
}

Tab* AIPrintAssistantPanel::target_tab() const
{
    if (selected_scope() == AI::Scope::Object)
        return nullptr;
    auto* tab = wxGetApp().get_tab(selected_scope() == AI::Scope::Process  ? Preset::TYPE_PRINT :
                                   selected_scope() == AI::Scope::Filament ? Preset::TYPE_FILAMENT :
                                                                             Preset::TYPE_PRINTER);
    if (!tab || !tab->get_config())
        throw std::runtime_error("The selected preset is unavailable.");
    return tab;
}

void AIPrintAssistantPanel::add_images()
{
    try {
        wxFileDialog dialog(this, _L("Attach screenshots or print photos"), wxEmptyString, wxEmptyString,
                            "PNG or JPEG images (*.png;*.jpg;*.jpeg)|*.png;*.jpg;*.jpeg", wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE);
        if (dialog.ShowModal() != wxID_OK)
            return;
        wxArrayString paths;
        dialog.GetPaths(paths);
        if (m_images.size() + paths.size() > AI::MAX_IMAGES)
            throw std::runtime_error("Attach at most four images.");
        std::vector<AI::ImageInput> attachments;
        std::vector<wxBitmap>       thumbnails;
        for (const auto& path : paths) {
            wxFile file(path);
            if (!file.IsOpened() || file.Length() <= 0 || file.Length() > 20 * 1024 * 1024)
                throw std::runtime_error("Choose PNG or JPEG files smaller than 20 MiB.");
            std::string original(size_t(file.Length()), '\0');
            if (file.Read(original.data(), original.size()) != original.size())
                throw std::runtime_error("Could not read the image file.");
            AI::validate_image_file(original);
            wxMemoryInputStream input(original.data(), original.size());
            wxImage             decoded;
            if (!decoded.LoadFile(input) || (decoded.GetType() != wxBITMAP_TYPE_PNG && decoded.GetType() != wxBITMAP_TYPE_JPEG))
                throw std::runtime_error("Could not read a PNG or JPEG image.");
            if (int64_t(decoded.GetWidth()) * decoded.GetHeight() > 40000000)
                throw std::runtime_error("Choose images of at most 40 megapixels.");
            const double scale = std::min(1., 1568. / std::max(decoded.GetWidth(), decoded.GetHeight()));
            if (scale < 1.)
                decoded.Rescale(std::max(1, int(decoded.GetWidth() * scale)), std::max(1, int(decoded.GetHeight() * scale)),
                                wxIMAGE_QUALITY_HIGH);
            // A new RGB image strips EXIF, file paths, and other original metadata.
            wxImage    clean(decoded.GetWidth(), decoded.GetHeight());
            const auto pixels = size_t(clean.GetWidth()) * clean.GetHeight();
            std::memcpy(clean.GetData(), decoded.GetData(), pixels * 3);
            if (decoded.HasAlpha()) {
                for (size_t i = 0; i < pixels; ++i)
                    for (size_t channel = 0; channel < 3; ++channel)
                        clean.GetData()[3 * i + channel] = static_cast<unsigned char>(
                            (int(clean.GetData()[3 * i + channel]) * decoded.GetAlpha()[i] + 255 * (255 - decoded.GetAlpha()[i])) / 255);
            }
            clean.SetOption(wxIMAGE_OPTION_QUALITY, 85);
            wxMemoryOutputStream output;
            if (!clean.SaveFile(output, wxBITMAP_TYPE_JPEG))
                throw std::runtime_error("Could not prepare the image attachment.");
            std::string bytes(output.GetSize(), '\0');
            output.CopyTo(bytes.data(), bytes.size());
            attachments.push_back({"image/jpeg", to_utf8(wxBase64Encode(bytes.data(), bytes.size())), clean.GetWidth(), clean.GetHeight()});
            const double thumb_scale = std::min(double(FromDIP(80)) / clean.GetWidth(), double(FromDIP(54)) / clean.GetHeight());
            thumbnails.emplace_back(clean.Scale(std::max(1, int(clean.GetWidth() * thumb_scale)),
                                                std::max(1, int(clean.GetHeight() * thumb_scale)), wxIMAGE_QUALITY_HIGH));
        }
        AI::image_context(attachments);
        for (size_t i = 0; i < attachments.size(); ++i) {
            m_images.push_back(std::move(attachments[i]));
            m_thumbnails.push_back(std::move(thumbnails[i]));
            m_image_list->Append(wxFileName(paths[i]).GetFullName());
        }
        m_image_list->SetSelection(int(m_images.size()) - 1);
        invalidate_proposal();
        preview_image();

    } catch (const std::exception& error) {
        show_error(error);
    }
}

void AIPrintAssistantPanel::remove_image()
{
    const int index = m_image_list->GetSelection();
    if (index == wxNOT_FOUND)
        return;
    m_images.erase(m_images.begin() + index);
    m_thumbnails.erase(m_thumbnails.begin() + index);
    m_image_list->Delete(index);
    if (!m_images.empty())
        m_image_list->SetSelection(std::min(index, int(m_images.size()) - 1));
    invalidate_proposal();
    preview_image();
}

void AIPrintAssistantPanel::preview_image()
{
    const int index = m_image_list->GetSelection();
    m_image_preview->SetBitmap(index == wxNOT_FOUND ? wxNullBitmap : m_thumbnails[size_t(index)]);
    m_remove_image->Enable(!m_result && index != wxNOT_FOUND);
    m_attachments->Show(!m_images.empty());
    m_add_images->Enable(!m_result && m_images.size() < AI::MAX_IMAGES);
    m_composer->Layout();
    Layout();
}

void AIPrintAssistantPanel::validate_preset_candidate(const DynamicPrintConfig& config)
{
    // Compose filament vectors exactly as the slicer does, without touching the
    // live presets or the tabs' pointers into their edited configurations.
    PresetBundle staged(*wxGetApp().preset_bundle);
    auto&        presets               = selected_scope() == AI::Scope::Process  ? staged.prints :
                                         selected_scope() == AI::Scope::Filament ? staged.filaments :
                                                                                   staged.printers;
    presets.get_edited_preset().config = config;
    const auto errors                  = staged.full_config().validate(true);
    if (!errors.empty())
        throw std::runtime_error("Preset validation failed for " + errors.begin()->first + ": " + errors.begin()->second);
}

void AIPrintAssistantPanel::commit_preset(DynamicPrintConfig config)
{
    validate_preset_candidate(config);
    auto*      tab             = target_tab();
    auto&      bundle          = *wxGetApp().preset_bundle;
    const auto before_print    = bundle.prints.get_edited_preset().config;
    const auto before_filament = bundle.filaments.get_edited_preset().config;
    const auto before_printer  = bundle.printers.get_edited_preset().config;
    const auto before_project  = bundle.project_config;
    auto       notify          = [&] {
        m_plater->on_config_change(bundle.full_config());
        if (auto* model_tab = dynamic_cast<TabPrintModel*>(wxGetApp().get_model_tab()))
            model_tab->update_model_config();
        m_plater->schedule_background_process();
        m_plater->update_title_dirty_status();
    };
    try {
        tab->load_config(config);
        if (*tab->get_config() != config || bundle.project_config != before_project ||
            (selected_scope() != AI::Scope::Process && bundle.prints.get_edited_preset().config != before_print) ||
            (selected_scope() != AI::Scope::Filament && bundle.filaments.get_edited_preset().config != before_filament) ||
            (selected_scope() != AI::Scope::Printer && bundle.printers.get_edited_preset().config != before_printer))
            throw std::runtime_error("These settings require additional dependent changes. The proposal was rolled back; include those "
                                     "changes in a new proposal.");
        notify();
    } catch (...) {
        bundle.prints.get_edited_preset().config    = before_print;
        bundle.filaments.get_edited_preset().config = before_filament;
        bundle.printers.get_edited_preset().config  = before_printer;
        bundle.project_config                       = before_project;
        for (const auto type : {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER}) {
            auto* changed_tab = wxGetApp().get_tab(type);
            changed_tab->reload_config();
            changed_tab->update_dirty();
        }
        notify();
        throw;
    }
}

AI::Context AIPrintAssistantPanel::current_context(ModelObject** object, const std::string& object_id, bool inspect)
{
    if (m_plater->printer_technology() != ptFFF)
        throw std::runtime_error("The print assistant currently supports filament printing only.");
    auto* plate    = m_plater->get_partplate_list().get_curr_plate();
    auto* selected = selected_scope() == AI::Scope::Object ?
                         AI::resolve_settings_object(plate ? plate->get_objects_on_this_plate() : ModelObjectPtrs{}, object_id) :
                         nullptr;
    if (selected_scope() == AI::Scope::Object && !selected && !inspect)
        throw std::runtime_error("Specify object_id from the active plate inventory before changing object settings.");
    auto inherited = wxGetApp().preset_bundle->full_config();
    // Match the plate settings layered into the object's effective configuration.
    if (plate && selected_scope() == AI::Scope::Object)
        inherited.apply(*plate->config());
    auto*      tab      = target_tab();
    const auto identity = (tab ? tab->m_presets->get_edited_preset().name : std::string()) +
                          ":plate:" + std::to_string(m_plater->get_partplate_list().get_curr_plate_index());
    const auto  images = AI::image_context(m_turn_images);
    AI::Context context;
    if (tab)
        context = AI::capture_preset_context(inherited, selected_scope(), *tab->get_config(), identity, images);
    else if (selected) {
        try {
            context = AI::capture_context(inherited, *selected, selected_scope(), tab ? tab->get_config() : nullptr, identity, images);
        } catch (const std::runtime_error& error) {
            if (!inspect)
                throw;
            context = AI::capture_read_context(inherited, selected, selected_scope(), tab ? tab->get_config() : nullptr, identity, images);
            context.public_context["editing_unavailable"] = error.what();
        }
    } else
        context = AI::capture_read_context(inherited, nullptr, selected_scope(), tab ? tab->get_config() : nullptr, identity, images);
    context.public_context["plate_context"] = ai_plate_context(*m_plater);
    context.public_context["read_only"]     = context.read_only;
    if (object != nullptr)
        *object = selected;
    return context;
}

void AIPrintAssistantPanel::refresh_config_ui(ModelObject& object)
{
    wxGetApp().obj_list()->object_config_options_changed({&object, nullptr});
    if (auto* tab = dynamic_cast<TabPrintModel*>(wxGetApp().get_model_tab()))
        tab->update_model_config();
    wxGetApp().params_panel()->notify_object_config_changed();
    m_plater->schedule_background_process();
    m_plater->update_title_dirty_status();
}

void AIPrintAssistantPanel::commit(ModelObject& object, DynamicPrintConfig config, const wxString& description)
{
    const ModelConfig        before   = static_cast<const ModelConfig&>(object.config);
    const DynamicPrintConfig expected = config;
    m_plater->take_snapshot(to_utf8(description));
    object.config.assign_config(std::move(config));
    try {
        refresh_config_ui(object);
        if (object.config.get() != expected)
            throw std::runtime_error("Refreshing the settings changed additional values. The assistant changes were rolled back.");
    } catch (...) {
        object.config.assign_config(before);
        refresh_config_ui(object);
        throw;
    }
}

void AIPrintAssistantPanel::export_context()
{
    try {
        if (!m_images.empty())
            m_turn_images = m_images;
        json exported = {{"model_context", ai_model_context(*m_plater).data},
                         {"model_tools", ai_model_tools(*m_plater)},
                         {"model_operation_schema", AI::model_operation_schema()},
                         {"proposal_schema", AI::proposal_schema()}};
        try {
            exported["context"] = current_context(nullptr, {}, true).public_context;
        } catch (const std::exception& error) {
            exported["settings_unavailable"] = error.what();
        }
        wxFileDialog dialog(this, _L("Export assistant context"), wxEmptyString, "assistant-context.json", "JSON files (*.json)|*.json",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK)
            return;
        // Export exactly the context sent to the provider, plus the proposal
        // schema for reproducible offline experiments. Never export full config.
        const auto text = exported.dump(2);
        wxFile     file(dialog.GetPath(), wxFile::write);
        if (!file.IsOpened() || file.Write(text.data(), text.size()) != text.size())
            throw std::runtime_error("Could not write the assistant context file.");
        m_status->SetLabel(_L("Context exported. A proposal must echo its context ID and object ID."));
    } catch (const std::exception& error) {
        show_error(error);
    }
}

void AIPrintAssistantPanel::import_proposal()
{
    try {
        wxFileDialog dialog(this, _L("Load assistant proposal"), wxEmptyString, wxEmptyString, "JSON files (*.json)|*.json",
                            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dialog.ShowModal() != wxID_OK)
            return;
        wxFile file(dialog.GetPath());
        if (!file.IsOpened() || file.Length() <= 0 || file.Length() > 64 * 1024)
            throw std::runtime_error("Choose a proposal JSON file smaller than 64 KiB.");
        std::string text(size_t(file.Length()), '\0');
        if (file.Read(text.data(), text.size()) != text.size())
            throw std::runtime_error("Could not read the proposal file.");
        const auto document = AI::parse_json(text);
        if (m_welcome)
            m_welcome->Hide();
        begin_activity();
        m_tool_summary.clear();
        const AI::ChatToolCall call{"offline", document.contains("operation") ? "propose_model_operation" : "propose_settings", document};
        try {
            if (document.contains("operation"))
                display_model_operation(AI::parse_model_operation(document));
            else
                display_proposal(AI::parse_proposal(text));
            add_tool_card(call, m_last_tool_result, false);
        } catch (const std::exception& error) {
            add_tool_card(call, {{"message", error.what()}}, true);
            finish_activity(_L("Activity"));
            throw;
        }
        finish_activity(_L("Activity"));
        layout_chat();
    } catch (const std::exception& error) {
        show_error(error);
    }
}

} // namespace
wxWindow* create_ai_print_assistant(wxWindow* parent, Plater* plater, std::function<void()> close)
{
    return new AIPrintAssistantPanel(parent, plater, std::move(close));
}
void suspend_ai_print_assistant(wxWindow* panel) { static_cast<AIPrintAssistantPanel*>(panel)->suspend(); }
void update_ai_print_assistant_theme(wxWindow* panel)
{
    if (panel)
        static_cast<AIPrintAssistantPanel*>(panel)->update_theme();
}
} // namespace Slic3r::GUI
