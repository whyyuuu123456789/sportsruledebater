// Model access for the council and the video review. Ollama models (ollama_chat/<name> or ollama/<name>) are called
// through Ollama's HTTP API; Claude models (anthropic/<name>) through the Anthropic Messages API, with the key in
// ANTHROPIC_API_KEY.
//
//     LLM_MODEL      the advocates and the case builder (default ollama_chat/llama3.1:8b)
//     VISION_MODEL   watches the video frames and sits on the panel (default ollama_chat/qwen2.5vl:7b)

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "util.hpp"

namespace llm {

extern const std::string MODEL;
extern const std::string VISION_MODEL;
extern const std::string OLLAMA;

using Images = std::vector<std::string>;  // JPEG bytes
// chat(system, user, max_tokens, seed, images, json_mode) -> the model's reply
using Chat = std::function<std::string(const std::string& system, const std::string& user, int max_tokens, int seed,
                                       const Images& images, bool json_mode)>;

// The model can't be used: Ollama isn't running, the model isn't downloaded, or there's no API key.
struct Unavailable : RuntimeError { using RuntimeError::RuntimeError; };

void wait_for_ollama(double timeout = 30);
Chat connect_to(const std::string& model);
// The chat function for one model. Tests replace it with a stand-in.
extern std::function<Chat(const std::string& model)> connect;

std::string base64(std::string_view data);

}  // namespace llm
