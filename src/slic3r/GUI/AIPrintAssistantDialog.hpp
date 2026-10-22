#ifndef slic3r_GUI_AIPrintAssistantDialog_hpp_
#define slic3r_GUI_AIPrintAssistantDialog_hpp_

#include <functional>
class wxWindow;

namespace Slic3r::GUI {
class Plater;
wxWindow* create_ai_print_assistant(wxWindow* parent, Plater* plater, std::function<void()> close);
void      suspend_ai_print_assistant(wxWindow* panel);
void      update_ai_print_assistant_theme(wxWindow* panel);
} // namespace Slic3r::GUI

#endif
