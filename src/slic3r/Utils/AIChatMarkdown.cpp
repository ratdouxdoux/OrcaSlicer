#include "AIChatMarkdown.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace Slic3r::AIPrintAssistant {
namespace {
using View = std::string_view;

bool space(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }
View trim(View text)
{
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return text;
}
std::string escape(View text)
{
    std::string out;
    for (char ch : text) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += ch;
        }
    }
    return out;
}
size_t run(View text, size_t pos, char marker)
{
    size_t end = pos;
    while (end < text.size() && text[end] == marker)
        ++end;
    return end - pos;
}
bool word(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || static_cast<unsigned char>(ch) >= 128; }

std::string inline_html(View text, unsigned depth = 0)
{
    if (depth >= 8)
        return escape(text);
    std::string out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '\\' && i + 1 < text.size() && std::ispunct(static_cast<unsigned char>(text[i + 1]))) {
            out += escape(text.substr(i + 1, 1));
            i += 2;
            continue;
        }
        if (text[i] == '`') {
            const auto length = run(text, i, '`');
            const auto marker = text.substr(i, length);
            size_t     end    = i + length;
            while ((end = text.find(marker, end)) != View::npos && run(text, end, '`') != length)
                end += run(text, end, '`');
            if (end != View::npos) {
                auto code = text.substr(i + length, end - i - length);
                if (code.size() > 2 && code.front() == ' ' && code.back() == ' ' && !trim(code).empty())
                    code = code.substr(1, code.size() - 2);
                out += "<tt>" + escape(code) + "</tt>";
                i = end + length;
                continue;
            }
        }
        const bool image = text[i] == '!' && i + 1 < text.size() && text[i + 1] == '[';
        if (text[i] == '[' || image) {
            const size_t begin = i + (image ? 2 : 1);
            const auto   close = text.find("](", begin);
            if (close != View::npos) {
                size_t end = close + 2, nesting = 1;
                for (; end < text.size(); ++end) {
                    if (text[end] == '\\' && end + 1 < text.size()) {
                        ++end;
                        continue;
                    }
                    if (text[end] == '(')
                        ++nesting;
                    if (text[end] == ')' && --nesting == 0)
                        break;
                }
                if (end < text.size()) {
                    const auto url   = trim(text.substr(close + 2, end - close - 2));
                    const auto label = inline_html(text.substr(begin, close - begin), depth + 1);
                    if (image)
                        out += "[Image: " + label + "]";
                    else if (chat_markdown_web_link(url))
                        out += "<a href=\"" + escape(url) + "\">" + label + "</a>";
                    else
                        out += label + " (" + escape(url) + ")";
                    i = end + 1;
                    continue;
                }
            }
        }
        if (text[i] == '*' || text[i] == '_') {
            const char   marker     = text[i];
            const size_t count      = std::min(run(text, i, marker), size_t(3));
            const bool   intra_word = marker == '_' && i > 0 && word(text[i - 1]);
            if (!intra_word && i + count < text.size() && !space(text[i + count])) {
                size_t end = i + count;
                while ((end = text.find(text.substr(i, count), end)) != View::npos) {
                    if (end > i + count && !space(text[end - 1]) && !(marker == '_' && end + count < text.size() && word(text[end + count])))
                        break;
                    end += count;
                }
                if (end != View::npos) {
                    const std::string open  = count == 3 ? "<b><i>" : count == 2 ? "<b>" : "<i>";
                    const std::string close = count == 3 ? "</i></b>" : count == 2 ? "</b>" : "</i>";
                    out += open + inline_html(text.substr(i + count, end - i - count), depth + 1) + close;
                    i = end + count;
                    continue;
                }
            }
        }
        out += text[i] == '\n' ? "<br>" : escape(text.substr(i, 1));
        ++i;
    }
    return out;
}

std::vector<View> table_cells(View line)
{
    line = trim(line);
    if (!line.empty() && line.front() == '|')
        line.remove_prefix(1);
    std::vector<View> cells;
    size_t            start = 0, ticks = 0;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            ++i;
            continue;
        }
        if (line[i] == '`') {
            const auto n = run(line, i, '`');
            if (!ticks)
                ticks = n;
            else if (ticks == n)
                ticks = 0;
            i += n - 1;
        } else if (line[i] == '|' && !ticks) {
            cells.push_back(trim(line.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (start < line.size() || cells.empty())
        cells.push_back(trim(line.substr(start)));
    return cells;
}
bool table_separator(View line, size_t columns)
{
    const auto cells = table_cells(line);
    if (cells.size() != columns || line.find('|') == View::npos)
        return false;
    return std::all_of(cells.begin(), cells.end(), [](View cell) {
        if (!cell.empty() && cell.front() == ':')
            cell.remove_prefix(1);
        if (!cell.empty() && cell.back() == ':')
            cell.remove_suffix(1);
        return cell.size() >= 3 && cell.find_first_not_of('-') == View::npos;
    });
}
unsigned heading(View text)
{
    const auto n = run(text, 0, '#');
    return n >= 1 && n <= 6 && (n == text.size() || space(text[n])) ? unsigned(n) : 0;
}
bool rule(View line)
{
    if (line.empty() || (line.front() != '-' && line.front() != '*' && line.front() != '_'))
        return false;
    const auto marker = line.front();
    unsigned   count  = 0;
    for (auto ch : line) {
        if (ch == marker)
            ++count;
        else if (!space(ch))
            return false;
    }
    return count >= 3;
}
struct ListMarker
{
    size_t   indent = 0, content = 0;
    unsigned number  = 0;
    bool     ordered = false;
};
ListMarker list_marker(View line)
{
    ListMarker marker;
    while (marker.indent < line.size() && line[marker.indent] == ' ')
        ++marker.indent;
    size_t i = marker.indent;
    if (i == line.size())
        return marker;
    if (line[i] == '-' || line[i] == '*' || line[i] == '+')
        ++i;
    else {
        marker.ordered = true;
        while (i < line.size() && i - marker.indent < 9 && line[i] >= '0' && line[i] <= '9') {
            marker.number = marker.number * 10 + (line[i++] - '0');
        }
        if (i == marker.indent || i == line.size() || (line[i] != '.' && line[i] != ')'))
            return marker;
        ++i;
    }
    if (i < line.size() && space(line[i]))
        marker.content = i + 1;
    return marker;
}

class Renderer
{
public:
    explicit Renderer(const ChatMarkdownStyle& style) : style(style) {}
    std::string blocks(const std::vector<View>& lines, unsigned depth = 0) const
    {
        std::string out;
        for (size_t i = 0; i < lines.size();) {
            const auto line = trim(lines[i]);
            if (line.empty()) {
                ++i;
                continue;
            }
            const char   fence      = line.front();
            const size_t fence_size = (fence == '`' || fence == '~') ? run(line, 0, fence) : 0;
            if (fence_size >= 3) {
                ++i;
                std::string  code;
                const size_t start = i;
                while (i < lines.size()) {
                    const auto next = trim(lines[i]);
                    if (run(next, 0, fence) >= fence_size && trim(next.substr(run(next, 0, fence))).empty()) {
                        ++i;
                        break;
                    }
                    if (i != start)
                        code += "\n";
                    code += lines[i++];
                }
                out += "<p><table width=\"100%\" cellspacing=\"0\" cellpadding=\"8\" bgcolor=\"" + style.inset + "\"><tr><td><tt>" +
                       code_html(code) + "</tt></td></tr></table></p>";
                continue;
            }
            const auto headers = table_cells(line);
            if (i + 1 < lines.size() && table_separator(trim(lines[i + 1]), headers.size())) {
                const auto alignment = table_cells(trim(lines[i + 1]));
                const bool stacked   = style.compact_tables && headers.size() > 2;
                out += "<p><table width=\"100%\" border=\"0\" cellspacing=\"1\" cellpadding=\"6\">";
                if (!stacked) {
                    out += "<tr bgcolor=\"" + style.inset + "\">";
                    for (auto header : headers)
                        out += "<td><b>" + inline_html(header) + "</b></td>";
                    out += "</tr>";
                }
                i += 2;
                size_t rows = 0;
                while (i < lines.size() && !trim(lines[i]).empty() && lines[i].find('|') != View::npos) {
                    const auto cells = table_cells(lines[i++]);
                    out += "<tr bgcolor=\"" + (rows++ % 2 ? style.inset : style.background) + "\">";
                    if (stacked)
                        out += "<td>";
                    for (size_t col = 0; col < headers.size(); ++col) {
                        const auto value = col < cells.size() ? inline_html(cells[col]) : std::string();
                        if (stacked) {
                            if (col)
                                out += "<br>";
                            out += "<b>" + inline_html(headers[col]) + ":</b> " + value;
                        } else {
                            const std::string align = alignment[col].back() == ':' ? (alignment[col].front() == ':' ? "center" : "right") :
                                                                                     "left";
                            out += "<td valign=\"top\" align=\"" + align + "\">" + value + "</td>";
                        }
                    }
                    out += stacked ? "</td></tr>" : "</tr>";
                }
                out += "</table></p>";
                continue;
            }
            const auto level = heading(line);
            if (level) {
                // wxHtmlWindow renders H4/H6 in italics; use consistent bold headings.
                const auto size = std::to_string(level < 4 ? 7 - level : 3);
                out += "<p><font size=\"" + size + "\"><b>" + inline_html(trim(line.substr(level))) + "</b></font></p>";
                ++i;
                continue;
            }
            if (rule(line)) {
                out += "<hr>";
                ++i;
                continue;
            }
            if (depth < 8 && line.front() == '>') {
                std::vector<View> quote;
                while (i < lines.size() && !trim(lines[i]).empty() && trim(lines[i]).front() == '>')
                    quote.push_back(trim(trim(lines[i++]).substr(1)));
                out += "<blockquote>" + blocks(quote, depth + 1) + "</blockquote>";
                continue;
            }
            const auto marker = list_marker(lines[i]);
            if (depth < 8 && marker.content) {
                const std::string tag = marker.ordered ? "ol" : "ul";
                out += "<" + tag + (marker.ordered ? " start=\"" + std::to_string(marker.number) + "\"" : "") + ">";
                while (i < lines.size()) {
                    const auto item = list_marker(lines[i]);
                    if (!item.content || item.indent != marker.indent || item.ordered != marker.ordered)
                        break;
                    std::vector<View> contents{lines[i++].substr(item.content)};
                    while (i < lines.size()) {
                        if (trim(lines[i]).empty()) {
                            if (i + 1 < lines.size() && list_marker(lines[i + 1]).indent > marker.indent) {
                                contents.push_back({});
                                ++i;
                                continue;
                            }
                            break;
                        }
                        const auto next = list_marker(lines[i]);
                        if (next.indent <= marker.indent)
                            break;
                        contents.push_back(lines[i++].substr(std::min(item.content, next.indent)));
                    }
                    out += "<li>" + blocks(contents, depth + 1) + "</li>";
                    if (i < lines.size() && trim(lines[i]).empty() && i + 1 < lines.size() && list_marker(lines[i + 1]).content)
                        ++i;
                }
                out += "</" + tag + ">";
                continue;
            }
            std::string paragraph;
            do {
                if (!paragraph.empty())
                    paragraph += "\n";
                paragraph += trim(lines[i++]);
                if (i == lines.size() || trim(lines[i]).empty())
                    break;
                const auto next = trim(lines[i]);
                if (heading(next) || rule(next) || next.front() == '>' || list_marker(lines[i]).content || run(next, 0, '`') >= 3 ||
                    run(next, 0, '~') >= 3 || (i + 1 < lines.size() && table_separator(trim(lines[i + 1]), table_cells(next).size())))
                    break;
            } while (true);
            out += "<p>" + inline_html(paragraph) + "</p>";
        }
        return out;
    }

private:
    const ChatMarkdownStyle& style;
    std::string              code_html(View text) const
    {
        std::string out;
        unsigned    column = 0;
        for (size_t i = 0; i < text.size();) {
            if (text[i] == '\n') {
                out += "<br>";
                column = 0;
                ++i;
                continue;
            }
            if (column >= std::max(style.code_columns, 16u)) {
                out += "<br>";
                column = 0;
            }
            size_t end = i + 1;
            while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
                ++end;
            out += text[i] == ' ' ? "&nbsp;" : text[i] == '\t' ? "&nbsp;&nbsp;&nbsp;&nbsp;" : escape(text.substr(i, end - i));
            column += text[i] == '\t' ? 4 : 1;
            i = end;
        }
        return out;
    }
};
} // namespace

bool chat_markdown_web_link(View url)
{
    if (url.substr(0, 8) != "https://" && url.substr(0, 7) != "http://")
        return false;
    if (url.size() > 4096 || url.size() <= url.find("://") + 3)
        return false;
    return std::none_of(url.begin(), url.end(),
                        [](unsigned char ch) { return ch <= 32 || ch == 127 || ch == '\\' || ch == '"' || ch == '<' || ch == '>'; });
}

std::string chat_markdown_html(View text, const ChatMarkdownStyle& style)
{
    std::vector<View> lines;
    size_t            begin = 0;
    for (;;) {
        const auto end  = text.find('\n', begin);
        auto       line = text.substr(begin, end == View::npos ? end : end - begin);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        if (end == View::npos)
            break;
        begin = end + 1;
    }
    return "<html><body bgcolor=\"" + style.background + "\" text=\"" + style.foreground + "\" link=\"" + style.link + "\"><font color=\"" +
           style.foreground + "\">" + Renderer(style).blocks(lines) + "</font></body></html>";
}
} // namespace Slic3r::AIPrintAssistant
