#include "llm.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "http.hpp"
#include "json.hpp"

namespace llm {

namespace {

std::string env(const char* name, const char* fallback) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

std::string after_slash(const std::string& model) {
    size_t slash = model.find('/');
    return slash == std::string::npos ? model : model.substr(slash + 1);
}

json::Value ollama(const std::string& path, const json::Value& body, long timeout = 600) {
    auto resp = http::urlopen(OLLAMA + path, "POST", body.dump(), {"Content-Type: application/json"}, timeout);
    return json::Value::parse(resp.body);
}

std::string ask_ollama(const std::string& name, const std::string& system, const std::string& user, int max_tokens,
                       int seed, const Images& images, bool json_mode) {
    json::Array pics;
    for (const auto& jpeg : images) pics.emplace_back(base64(jpeg));
    int num_ctx = pics.empty() ? 8192 : 16384;  // frames take a few hundred tokens each
    json::Object message{{"role", "user"}, {"content", user}};
    if (!pics.empty()) message["images"] = pics;
    json::Value body = json::Object{
        {"model", name},
        {"stream", false},
        {"messages", json::Array{json::Object{{"role", "system"}, {"content", system}}, message}},
        {"options", json::Object{{"temperature", 0.3}, {"seed", seed}, {"num_predict", max_tokens}, {"num_ctx", num_ctx}}},
    };
    if (json_mode) body["format"] = "json";
    return ollama("/api/chat", body).get("message").get("content").as_string();
}

http::Headers anthropic_headers() {
    http::Headers headers{"Content-Type: application/json", "anthropic-version: 2023-06-01"};
    if (const char* key = std::getenv("ANTHROPIC_API_KEY"); key && *key) {
        headers.push_back(std::string("x-api-key: ") + key);
    } else if (const char* token = std::getenv("ANTHROPIC_AUTH_TOKEN"); token && *token) {
        headers.push_back(std::string("Authorization: Bearer ") + token);
        headers.push_back("anthropic-beta: oauth-2025-04-20");
    }
    return headers;
}

std::string ask_anthropic(const std::string& name, const std::string& system, const std::string& user, int max_tokens,
                          const Images& images) {
    json::Array content;
    for (const auto& jpeg : images)
        content.emplace_back(json::Object{
            {"type", "image"},
            {"source", json::Object{{"type", "base64"}, {"media_type", "image/jpeg"}, {"data", base64(jpeg)}}}});
    content.emplace_back(json::Object{{"type", "text"}, {"text", user}});
    // The prompts ask for short replies themselves. Current Claude models think before they answer, and that comes
    // out of max_tokens, so the small limits meant for local models would cut the reply off.
    json::Value body = json::Object{
        {"model", name},
        {"max_tokens", std::max(max_tokens, 16000)},
        {"system", system},
        {"messages", json::Array{json::Object{{"role", "user"}, {"content", content}}}},
    };
    std::string base = env("ANTHROPIC_BASE_URL", "https://api.anthropic.com");
    auto resp = http::request(base + "/v1/messages", "POST", body.dump(), anthropic_headers(), 600);
    json::Value data;
    try {
        data = json::Value::parse(resp.body);
    } catch (const json::ParseError&) {
        throw http::HttpError(resp.status, resp.reason, resp.body);
    }
    if (resp.status >= 400) {
        const json::Value& message = data.get("error").get("message");
        throw http::HttpError(resp.status, message.is_string() ? message.as_string() : resp.reason, resp.body);
    }
    std::string text;  // a refusal or a reply cut off by max_tokens comes back with little or no text
    for (const auto& block : json::iterate(data.get("content")))
        if (block.get("type") == json::Value("text") && block.get("text").is_string()) text += block.get("text").as_string();
    return text;
}

}  // namespace

const std::string MODEL = env("LLM_MODEL", "ollama_chat/llama3.1:8b");
const std::string VISION_MODEL = env("VISION_MODEL", "ollama_chat/qwen2.5vl:7b");
const std::string OLLAMA = env("OLLAMA_API_BASE", "http://localhost:11434");

std::function<Chat(const std::string& model)> connect = connect_to;

void wait_for_ollama(double timeout) {
    double deadline = now() + timeout;
    while (now() < deadline) {
        try {
            http::urlopen(OLLAMA + "/api/version", "GET", "", {}, 2);
            return;
        } catch (const OSError&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    throw Unavailable("Couldn't reach Ollama at " + OLLAMA + ". Start it with: brew services start ollama");
}

Chat connect_to(const std::string& model) {
    bool local = model.rfind("ollama", 0) == 0;
    std::string name = after_slash(model);
    if (local) {
        wait_for_ollama();
        try {
            ollama("/api/show", json::Object{{"model", name}}, 30);
        } catch (const http::HttpError&) {
            throw Unavailable("Ollama doesn't have " + name + ". Download it with: ollama pull " + name);
        }
        return [name](const std::string& system, const std::string& user, int max_tokens, int seed,
                      const Images& images, bool json_mode) {
            return ask_ollama(name, system, user, max_tokens, seed, images, json_mode);
        };
    }
    if (model.rfind("anthropic/", 0) == 0) {
        if (!std::getenv("ANTHROPIC_API_KEY") && !std::getenv("ANTHROPIC_AUTH_TOKEN"))
            throw Unavailable(model + " needs an Anthropic API key: set ANTHROPIC_API_KEY.");
        return [name](const std::string& system, const std::string& user, int max_tokens, int, const Images& images,
                      bool) { return ask_anthropic(name, system, user, max_tokens, images); };
    }
    throw Unavailable(model + " isn't a model this build can use. Use an Ollama model (ollama_chat/<name>) or a Claude "
                      "model (anthropic/<name>).");
}

std::string base64(std::string_view data) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t n = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) |
                     static_cast<unsigned char>(data[i + 2]);
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += table[(n >> 6) & 63];
        out += table[n & 63];
    }
    if (i < data.size()) {
        uint32_t n = static_cast<unsigned char>(data[i]) << 16;
        if (i + 1 < data.size()) n |= static_cast<unsigned char>(data[i + 1]) << 8;
        out += table[n >> 18];
        out += table[(n >> 12) & 63];
        out += i + 1 < data.size() ? table[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace llm
