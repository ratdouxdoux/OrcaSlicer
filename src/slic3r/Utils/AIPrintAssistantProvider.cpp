#include "AIPrintAssistantProvider.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <vector>
#include <openssl/sha.h>

namespace Slic3r::AIPrintAssistant {

void validate_image_file(const std::string& bytes)
{
    if (bytes.empty() || bytes.size() > 20 * 1024 * 1024)
        throw std::runtime_error("Choose PNG or JPEG files smaller than 20 MiB.");
    auto byte    = [&](size_t pos) { return static_cast<unsigned char>(bytes.at(pos)); };
    auto bounded = [](uint64_t width, uint64_t height) {
        if (width == 0 || height == 0 || width > 40000000 || height > 40000000 || width * height > 40000000)
            throw std::runtime_error("Choose images of at most 40 megapixels.");
    };
    if (bytes.size() >= 24 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0 && bytes.compare(12, 4, "IHDR") == 0) {
        auto big_endian = [&](size_t pos) {
            return uint64_t(byte(pos)) << 24 | uint64_t(byte(pos + 1)) << 16 | uint64_t(byte(pos + 2)) << 8 | byte(pos + 3);
        };
        bounded(big_endian(16), big_endian(20));
        return;
    }
    if (bytes.size() >= 4 && byte(0) == 0xff && byte(1) == 0xd8) {
        size_t pos = 2;
        while (pos + 4 < bytes.size()) {
            if (byte(pos++) != 0xff)
                break;
            while (pos < bytes.size() && byte(pos) == 0xff)
                ++pos;
            if (pos + 3 >= bytes.size())
                break;
            const auto marker = byte(pos++);
            if (marker == 0xda || marker == 0xd9)
                break;
            const size_t length = size_t(byte(pos)) * 256 + byte(pos + 1);
            if (length < 2 || length > bytes.size() - pos)
                break;
            if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc && length >= 8) {
                bounded(size_t(byte(pos + 5)) * 256 + byte(pos + 6), size_t(byte(pos + 3)) * 256 + byte(pos + 4));
                return;
            }
            pos += length;
        }
    }
    throw std::runtime_error("Could not read a PNG or JPEG image header.");
}

nlohmann::json image_context(const std::vector<ImageInput>& images)
{
    if (images.size() > MAX_IMAGES)
        throw std::runtime_error("Attach at most four images.");
    auto result = nlohmann::json::array();
    for (const auto& image : images) {
        if ((image.media_type != "image/png" && image.media_type != "image/jpeg") || image.width <= 0 || image.height <= 0 ||
            image.width > 1568 || image.height > 1568 || image.base64.empty() || image.base64.size() > MAX_IMAGE_BASE64_BYTES ||
            image.base64.size() % 4 != 0)
            throw std::runtime_error("Invalid or oversized image attachment.");
        const auto padding = image.base64.find('=');
        for (size_t i = 0; i < image.base64.size(); ++i) {
            const unsigned char ch       = image.base64[i];
            const bool          alphabet = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '+' ||
                                  ch == '/';
            if ((!alphabet && ch != '=') ||
                (padding != std::string::npos && (padding < image.base64.size() - 2 || (i > padding && ch != '='))))
                throw std::runtime_error("Invalid image encoding.");
        }
        unsigned char digest[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(image.base64.data()), image.base64.size(), digest);
        const char* hex = "0123456789abcdef";
        std::string hash;
        for (auto byte : digest) {
            hash += hex[byte >> 4];
            hash += hex[byte & 15];
        }
        result.push_back({{"number", result.size() + 1},
                          {"width", image.width},
                          {"height", image.height},
                          {"media_type", image.media_type},
                          {"sha256", hash}});
    }
    return result;
}

ProviderInfo provider_info(Provider provider)
{
    switch (provider) {
    case Provider::OpenRouter: return {"OpenRouter", "https://openrouter.ai/api/v1/chat/completions", "deepseek/deepseek-v4.1-flash-1017"};
    case Provider::Anthropic: return {"Anthropic", "https://api.anthropic.com/v1/messages", "claude-sonnet-5-5"};
    case Provider::BigModel: return {"BigModel", "https://open.bigmodel.cn/api/paas/v4/chat/completions", "glm-5.3-flash"};
    case Provider::BigModelCodingPlan:
        return {"BigModel (Coding Plan)", "https://open.bigmodel.cn/api/coding/paas/v4/chat/completions", "glm-5.3-flash"};
    case Provider::DeepSeek: return {"DeepSeek", "https://api.deepseek.com/chat/completions", "deepseek-flash"};
    // Plain HTTP on the company network, as the gateway is published. It serves
    // deepseek-v4-pro, deepseek-flash, glm-5.3 and glm-5.3-flash.
    case Provider::Gateway: return {"Snapmaker AI Gateway", "http://ai-gateway.s.com/v1/chat/completions", "deepseek-flash"};
    }
    throw std::runtime_error("Unknown assistant provider.");
}

Upstream upstream(Provider provider, const std::string& model)
{
    if (is_bigmodel(provider) || (provider == Provider::Gateway && model.rfind("glm", 0) == 0))
        return Upstream::BigModel;
    if (provider == Provider::DeepSeek || (provider == Provider::Gateway && model.rfind("deepseek", 0) == 0))
        return Upstream::DeepSeek;
    return Upstream::Other;
}

const char* provider_id(Provider provider)
{
    switch (provider) {
    case Provider::OpenRouter: return "openrouter";
    case Provider::Anthropic: return "anthropic";
    case Provider::BigModel: return "bigmodel";
    case Provider::BigModelCodingPlan: return "bigmodel_coding_plan";
    case Provider::DeepSeek: return "deepseek";
    case Provider::Gateway: return "snapmaker_gateway";
    }
    throw std::runtime_error("Unknown assistant provider.");
}

void validate_model_id(Provider provider, const std::string& model)
{
    // The ID goes into the request body and the settings file; OpenRouter's has a vendor prefix.
    if (model.empty() || model.size() > 128 ||
        !std::all_of(model.begin(), model.end(),
                     [provider](unsigned char ch) {
                         return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                                ch == '_' || ch == '.' || (provider == Provider::OpenRouter && (ch == '/' || ch == ':'));
                     }) ||
        (provider == Provider::OpenRouter && (model.find('/') == std::string::npos || model.front() == '/' || model.back() == '/')))
        throw std::runtime_error(std::string("Enter a valid ") + provider_info(provider).name + " model ID.");
}

std::string provider_error(
    Provider provider, unsigned status, const std::string& body, const std::string& api_key, const std::string& transport_error)
{
    std::string message = std::string(provider_info(provider).name) +
                          (status == 0   ? ": The network request failed." :
                           status == 200 ? ": The response stopped with an error." :
                                           " rejected the request (HTTP " + std::to_string(status) + ").");
    if (status == 401)
        message += " Check the API key in Settings.";
    else if (status == 402)
        message += " Check your account credits.";
    else if (status == 429)
        message += " Try again shortly.";
    // The gateway's name resolves only inside the company network.
    if (status == 0 && provider == Provider::Gateway)
        message += " The gateway is reachable only on the company network. On the VPN, switch to the office network or set the DNS "
                   "server to 172.17.100.43.";

    std::string detail;
    try {
        const auto  envelope     = parse_json(body, 64 * 1024);
        const auto& error        = envelope.at("error");
        auto        read_message = [](const nlohmann::json& value) -> std::string {
            if (value.is_string())
                return value.get<std::string>();
            if (value.is_object() && value.contains("message") && value["message"].is_string())
                return value["message"].get<std::string>();
            return {};
        };
        detail = read_message(error);
        // OpenRouter wraps the upstream provider's explanation in metadata.raw.
        if (error.is_object() && error.contains("metadata") && error["metadata"].is_object()) {
            const auto& metadata = error["metadata"];
            if (metadata.contains("raw")) {
                try {
                    auto raw = metadata["raw"];
                    if (raw.is_string())
                        raw = parse_json(raw.get<std::string>(), 64 * 1024);
                    const auto upstream = read_message(raw.is_object() && raw.contains("error") ? raw["error"] : raw);
                    if (!upstream.empty() && upstream != detail)
                        detail += (detail.empty() ? "" : "\n") + upstream;
                } catch (const std::exception&) { /* Never display a raw HTML page or request dump. */
                }
            }
        }
    } catch (const std::exception&) { /* Keep the HTTP diagnosis for malformed or oversized responses. */
    }
    if (status == 0 && !transport_error.empty())
        detail += (detail.empty() ? "" : "\n") + transport_error;
    if (detail.empty())
        return message;

    if (!api_key.empty()) {
        size_t pos = 0;
        while ((pos = detail.find(api_key, pos)) != std::string::npos) {
            detail.replace(pos, api_key.size(), "[redacted]");
            pos += 10;
        }
    }
    static const std::regex secrets(R"((sk-[A-Za-z0-9_-]+)|(Bearer[ \t]+[A-Za-z0-9._~+/-]+=*)|(data:image/[^\s"'<>]+))", std::regex::icase);
    detail = std::regex_replace(detail, secrets, "[redacted]");
    // Curl diagnostics may mention a proxy URL containing userinfo.
    static const std::regex url_credentials(R"(([a-z][a-z0-9+.-]*://)[^/\s"'<>]*@)", std::regex::icase);
    detail = std::regex_replace(detail, url_credentials, "$1[redacted]@");
    for (char& ch : detail)
        if (static_cast<unsigned char>(ch) < 32 || ch == 127)
            ch = ' ';
    if (detail.size() > 1200) {
        size_t end = 1200;
        while (end > 0 && (static_cast<unsigned char>(detail[end]) & 0xc0) == 0x80)
            --end;
        detail.resize(end);
        detail += "...";
    }
    return message + "\n" + detail;
}

bool retryable_status(unsigned status)
{
    // 529 is Anthropic's "overloaded"; 520 to 524 are Cloudflare's origin failures.
    return status == 0 || status == 408 || status == 425 || status == 429 || status == 500 || status == 502 || status == 503 ||
           status == 504 || (status >= 520 && status <= 524) || status == 529;
}

bool retryable_stream_error(const nlohmann::json& event)
{
    if (!event.is_object())
        return false;
    // Anthropic: {"type":"error","error":{"type":"overloaded_error"}}. OpenRouter and
    // chat-completions providers: {"error":{"code":503,...}}, also inside a choice.
    std::vector<const nlohmann::json*> errors;
    if (event.contains("error"))
        errors.push_back(&event["error"]);
    if (event.contains("choices") && event["choices"].is_array())
        for (const auto& choice : event["choices"])
            if (choice.is_object() && choice.contains("error"))
                errors.push_back(&choice["error"]);
    for (const auto* error : errors) {
        if (!error->is_object())
            continue;
        const auto type = error->value("type", "");
        if (type == "overloaded_error" || type == "api_error" || type == "rate_limit_error" || type == "timeout_error")
            return true;
        const auto code = error->contains("code") && (*error)["code"].is_number_integer() ? (*error)["code"].get<long long>() : 0;
        if (code >= 100 && code < 600 && retryable_status(unsigned(code)))
            return true;
    }
    return false;
}

int retry_delay_seconds(int attempt, const std::string& headers)
{
    // Headers accumulate across redirects; the last Retry-After is the final response's.
    static const std::regex retry_after(R"((?:^|\n)retry-after:[ \t]*(\d{1,6})[ \t]*\r?(?:\n|$))", std::regex::icase);
    int  seconds = -1;
    for (std::sregex_iterator it(headers.begin(), headers.end(), retry_after), end; it != end; ++it)
        seconds = std::stoi((*it)[1].str());
    if (seconds >= 0)
        return std::clamp(seconds, 1, 60);
    static constexpr int delays[] = {2, 5, 15};
    return delays[std::clamp(attempt, 0, int(std::size(delays)) - 1)];
}

} // namespace Slic3r::AIPrintAssistant
