#ifndef slic3r_AIChatMarkdown_hpp_
#define slic3r_AIChatMarkdown_hpp_

#include <string>
#include <string_view>

namespace Slic3r::AIPrintAssistant {
struct ChatMarkdownStyle
{
    std::string background     = "#FFFFFF";
    std::string foreground     = "#262E30";
    std::string inset          = "#F8F8F8";
    std::string link           = "#009688";
    bool        compact_tables = false;
    unsigned    code_columns   = 80;
};

// Native wxHtmlWindow-compatible chat Markdown: blocks, lists, tables and inline
// formatting. Raw HTML is displayed as text; images never fetch remote content.
std::string chat_markdown_html(std::string_view text, const ChatMarkdownStyle& style = {});
bool        chat_markdown_web_link(std::string_view url);
} // namespace Slic3r::AIPrintAssistant
#endif
