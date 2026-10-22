#include <catch2/catch_test_macros.hpp>
#include "slic3r/Utils/AIChatMarkdown.hpp"

namespace AI = Slic3r::AIPrintAssistant;

TEST_CASE("Assistant renders the reported settings table with headings and formatting", "[AIPrintAssistant][fast]")
{
    const auto html = AI::chat_markdown_html(
        "Done — here is the preview.\n\n## Previewed changes (object scope)\n\n"
        "| Setting | Before → Now | Why |\n|---|---|---|\n"
        "| **Enable support** | off → **on** | The overhang needs support |\n"
        "| Top Z distance | 0.08 → **0.16 mm** | Easier removal |\n\n"
        "Bonus: `flush_into_support` is already on.\n\n## Remaining changes\n- Switch scope\n- Slice again");
    CHECK(html.find("<font size=\"5\"><b>Previewed changes (object scope)</b></font>") != std::string::npos);
    CHECK(html.find("<b>Before → Now</b>") != std::string::npos);
    CHECK(html.find("<b>0.16 mm</b>") != std::string::npos);
    CHECK(html.find("<tt>flush_into_support</tt>") != std::string::npos);
    CHECK(html.find("<ul>") != std::string::npos);
    CHECK(html.find("|---|") == std::string::npos);
    CHECK(html.find("## Previewed") == std::string::npos);
}

TEST_CASE("Assistant Markdown distinguishes literal identifiers escaped delimiters and code", "[AIPrintAssistant][fast]")
{
    const auto html = AI::chat_markdown_html(
        "wall_loops and min_feature_size, *italic*, **bold**, ***both***, ~~removed~~ and _emphasis_.\n"
        "\\*literal\\* and ``a `tick` **literal**``\n\n"
        "```gcode\nG1 X20 E0.5 ; <not html> **not bold**\n```\nAfter code");
    CHECK(html.find("wall_loops and min_feature_size") != std::string::npos);
    CHECK(html.find("<i>italic</i>") != std::string::npos);
    CHECK(html.find("<b><i>both</i></b>") != std::string::npos);
    // wxHtmlWindow cannot render strikethrough; preserve its markers rather than silently dropping the meaning.
    CHECK(html.find("~~removed~~") != std::string::npos);
    CHECK(html.find("*literal*") != std::string::npos);
    CHECK(html.find("<tt>a `tick` **literal**</tt>") != std::string::npos);
    CHECK(html.find("&lt;not&nbsp;html&gt;") != std::string::npos);
    CHECK(html.find("**not&nbsp;bold**") != std::string::npos);
    CHECK(html.find("<p>After code</p>") != std::string::npos);
}

TEST_CASE("Assistant Markdown does not load HTML images files or executable links", "[AIPrintAssistant][fast]")
{
    const auto html = AI::chat_markdown_html(
        "<img src='https://example.org/tracker'> <script>bad()</script>\n\n"
        "![image](https://example.org/remote.png) [file](file:///private.txt) [run](javascript:alert(1)) "
        "[safe](https://example.org/help?q=1&b=2)");
    CHECK(html.find("<img") == std::string::npos);
    CHECK(html.find("<script") == std::string::npos);
    CHECK(html.find("[Image: image]") != std::string::npos);
    CHECK(html.find("href=\"file:") == std::string::npos);
    CHECK(html.find("href=\"javascript:") == std::string::npos);
    CHECK(html.find("href=\"https://example.org/help?q=1&amp;b=2\"") != std::string::npos);
    for (const auto* url :
         {"file:///C:/private", "javascript:alert(1)", "https://", "https://a\nmalicious", "https://a\\b", "data:text/html,bad"})
        CHECK_FALSE(AI::chat_markdown_web_link(url));
    CHECK(AI::chat_markdown_web_link("https://example.org/help"));
}

TEST_CASE("Assistant tables preserve escaped and inline-code pipes and adapt to narrow panels", "[AIPrintAssistant][fast]")
{
    const std::string text   = "| Setting | Value | Why |\n| :--- | :---: | ---: |\n| `a|b` | x\\|y | A reason |\n";
    const auto        normal = AI::chat_markdown_html(text);
    CHECK(normal.find("<tt>a|b</tt>") != std::string::npos);
    CHECK(normal.find("x|y") != std::string::npos);
    CHECK(normal.find("align=\"center\"") != std::string::npos);
    CHECK(normal.find("align=\"right\"") != std::string::npos);
    AI::ChatMarkdownStyle narrow;
    narrow.compact_tables = true;
    narrow.background     = "#202020";
    narrow.foreground     = "#EFEFEF";
    narrow.inset          = "#292929";
    const auto compact    = AI::chat_markdown_html(text, narrow);
    CHECK(compact.find("<b>Setting:</b> <tt>a|b</tt>") != std::string::npos);
    CHECK(compact.find("<b>Why:</b> A reason") != std::string::npos);
    CHECK(compact.find("bgcolor=\"#202020\"") != std::string::npos);
    CHECK(compact.find("color=\"#EFEFEF\"") != std::string::npos);
}

TEST_CASE("Assistant Markdown supports nested lists quotes and incomplete streamed blocks", "[AIPrintAssistant][fast]")
{
    const auto html = AI::chat_markdown_html("1. First\n   - Nested\n   - Second nested\n2. Next\n\n> A quote\n> with **bold**\n\n---");
    CHECK(html.find("<ol start=\"1\">") != std::string::npos);
    CHECK(html.find("<ul>") != std::string::npos);
    CHECK(html.find("<blockquote><p>A quote<br>with <b>bold</b></p></blockquote>") != std::string::npos);
    CHECK(html.find("<hr>") != std::string::npos);
    const std::string streamed = "## A heading\n\n| Setting | Value |\n|---|---|\n| Width | **0.4 mm** |\n\n```gcode\nG1 X20\n```";
    for (size_t length = 0; length <= streamed.size(); ++length) {
        const auto partial = AI::chat_markdown_html(std::string_view(streamed).substr(0, length));
        CHECK(partial.find("</body></html>") != std::string::npos);
    }
    const auto unfinished = AI::chat_markdown_html("```\n<b>still code\n");
    CHECK(unfinished.find("&lt;b&gt;still&nbsp;code") != std::string::npos);
    CHECK(unfinished.find("</tt></td></tr></table>") != std::string::npos);
    AI::ChatMarkdownStyle wrapped;
    wrapped.code_columns = 16;
    const auto code      = AI::chat_markdown_html("```\n\n1234567890123456é尾\n```", wrapped);
    CHECK(code.find("<tt><br>1234567890123456<br>é尾</tt>") != std::string::npos);
}
