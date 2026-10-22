#ifndef slic3r_AIPrintAssistantChat_hpp_
#define slic3r_AIPrintAssistantChat_hpp_

#include <map>
#include <set>
#include "AIPrintAssistantProvider.hpp"

namespace Slic3r::AIPrintAssistant {
struct ChatToolCall
{
    std::string    id;
    std::string    name;
    nlohmann::json arguments;
};

nlohmann::json chat_user_message(Provider provider, const std::string& text, const std::vector<ImageInput>& images = {});
nlohmann::json chat_turn_message(Provider                       provider,
                                 const std::string&             text,
                                 const std::vector<ImageInput>& images,
                                 const Context*                 context,
                                 const nlohmann::json&          plate_context = nlohmann::json::object());
nlohmann::json chat_tool_result(Provider provider, const ChatToolCall& call, const nlohmann::json& result, bool error = false);
std::string    make_chat_request(Provider provider, const std::string& model, const nlohmann::json& history);
nlohmann::json run_read_tool(const Context& context, const ChatToolCall& call);
std::string    read_tool_object_id(const ChatToolCall& call);

// Incremental SSE decoder. Tool arguments are available only after a verified
// terminal event and successful HTTP completion, never from partial JSON.
class ChatStream
{
public:
    explicit ChatStream(Provider provider, const std::string& api_key = {}) : m_provider(provider), m_api_key(api_key) {}
    void                      feed(const std::string& bytes);
    void                      finish() const;
    const std::string&        text() const { return m_text; }
    bool                      thinking() const { return m_thinking; }
    std::vector<std::string>  active_tools() const;
    std::vector<ChatToolCall> tool_calls() const;
    nlohmann::json            assistant_message() const;

private:
    void                          event(const std::string& data);
    Provider                      m_provider;
    std::string                   m_api_key;
    std::string                   m_line, m_event, m_text, m_stop;
    size_t                        m_bytes = 0;
    bool                          m_done = false, m_thinking = false;
    std::map<int, nlohmann::json> m_blocks;
    std::set<int>                 m_open_blocks;
    bool                          m_started = false;
    std::map<int, nlohmann::json> m_reasoning;
    std::string                   m_reasoning_content;
};
} // namespace Slic3r::AIPrintAssistant
#endif
