#pragma once

#include <array>
#include <string>
#include "libslic3r/AIPrintAssistant.hpp"

namespace Slic3r::AIPrintAssistant {

enum class Provider { OpenRouter, Anthropic, BigModel, BigModelCodingPlan, DeepSeek };
inline constexpr Provider                DEFAULT_PROVIDER = Provider::OpenRouter;
inline constexpr std::array<Provider, 5> PROVIDERS        = {Provider::OpenRouter, Provider::Anthropic, Provider::BigModel,
                                                             Provider::BigModelCodingPlan, Provider::DeepSeek};
inline constexpr bool is_bigmodel(Provider provider) { return provider == Provider::BigModel || provider == Provider::BigModelCodingPlan; }
inline constexpr bool uses_chat_completions(Provider provider)
{
    return provider == Provider::OpenRouter || provider == Provider::DeepSeek || is_bigmodel(provider);
}
inline constexpr bool uses_reasoning_content(Provider provider) { return is_bigmodel(provider) || provider == Provider::DeepSeek; }

struct ProviderInfo
{
    const char* name;
    const char* endpoint;
    const char* default_model;
};

ProviderInfo provider_info(Provider provider);
// Stable storage identifier, independent of display name and dropdown ordering.
const char* provider_id(Provider provider);
// Only display bounded provider/transport diagnostics, never the full response or credentials.
std::string provider_error(
    Provider provider, unsigned status, const std::string& body, const std::string& api_key = {}, const std::string& transport_error = {});

// Busy, overloaded, rate-limited and dropped requests are retried; a rejected
// key, missing credits or an invalid request are not. Status 0 is a network failure.
inline constexpr int MAX_RETRIES = 3;
bool                 retryable_status(unsigned status);
// The same for an error event inside a streamed response.
bool retryable_stream_error(const nlohmann::json& event);
// Seconds before retry number `attempt` (from 0): the provider's Retry-After
// header when it sends one, at most 60, else 2, 5 and 15.
int retry_delay_seconds(int attempt, const std::string& headers);

struct ImageInput
{
    std::string media_type;
    std::string base64;
    int         width  = 0;
    int         height = 0;
};
inline constexpr size_t MAX_IMAGES             = 4;
inline constexpr size_t MAX_IMAGE_BASE64_BYTES = 4 * 1024 * 1024;
nlohmann::json          image_context(const std::vector<ImageInput>& images);
// Check image headers before allocating decoded pixels. Only PNG/JPEG, <=40 MP.
void validate_image_file(const std::string& bytes);

// Pure protocol adapter. API keys belong only in the transport header.
std::string make_request(Provider                       provider,
                         const Context&                 context,
                         const std::string&             prompt,
                         const std::string&             model,
                         const std::vector<ImageInput>& images = {});
Proposal    parse_response(Provider provider, const std::string& body);
std::string make_openrouter_request(const Context&                 context,
                                    const std::string&             prompt,
                                    const std::string&             model,
                                    const std::vector<ImageInput>& images = {});
Proposal    parse_openrouter_response(const std::string& body);
std::string make_anthropic_request(const Context&                 context,
                                   const std::string&             prompt,
                                   const std::string&             model,
                                   const std::vector<ImageInput>& images = {});
Proposal    parse_anthropic_response(const std::string& body);

} // namespace Slic3r::AIPrintAssistant
