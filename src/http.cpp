#include "http.hpp"

#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <exception>

namespace http {

HttpError::HttpError(int code, const std::string& reason, std::string body)
    : OSError("HTTP Error " + std::to_string(code) + ": " + reason), code(code), body(std::move(body)) {}

namespace {

void ensure_curl() {
    static const bool ready = [] { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }();
    (void)ready;
}

// email.message.get_content_type(): "type/subtype" in lowercase, or text/plain if it's missing or malformed.
std::string media_type(std::string_view value) {
    std::string type = py::lower(py::strip(value.substr(0, value.find(';'))));
    return std::count(type.begin(), type.end(), '/') == 1 ? type : "text/plain";
}

struct Transfer {
    CURL* curl = nullptr;
    Response head;
    bool head_done = false;
    bool raise_status = false;
    bool stopped = false;  // on_head asked to stop before the body
    std::exception_ptr error;
    const std::function<bool(const Response&)>* on_head = nullptr;
    const std::function<void(std::string_view)>* on_data = nullptr;
};

size_t on_header(char* data, size_t size, size_t n, void* user) {
    auto* t = static_cast<Transfer*>(user);
    std::string line = py::rstrip(std::string_view(data, size * n), "\r\n");
    if (line.rfind("HTTP/", 0) == 0) {  // a new response begins, also after each redirect
        auto parts = py::split(line);
        t->head.status = parts.size() > 1 ? std::atoi(parts[1].c_str()) : 0;
        size_t at = line.find(' ', line.find(' ') + 1);
        t->head.reason = at == std::string::npos ? "" : line.substr(at + 1);
        t->head.content_type = "text/plain";
    } else if (size_t colon = line.find(':'); colon != std::string::npos) {
        if (py::lower(py::strip(line.substr(0, colon))) == "content-type")
            t->head.content_type = media_type(line.substr(colon + 1));
    }
    return size * n;
}

// Called once the final response's headers are in. Returns whether to go on to the body.
bool finish_head(Transfer* t) {
    if (t->head_done) return !t->stopped;
    t->head_done = true;
    char* url = nullptr;
    if (curl_easy_getinfo(t->curl, CURLINFO_EFFECTIVE_URL, &url) == CURLE_OK && url) t->head.url = url;
    if (t->raise_status && t->head.status >= 400) throw HttpError(t->head.status, t->head.reason);
    if (t->on_head && !(*t->on_head)(t->head)) t->stopped = true;
    return !t->stopped;
}

size_t on_body(char* data, size_t size, size_t n, void* user) {
    auto* t = static_cast<Transfer*>(user);
    try {  // exceptions can't cross libcurl, so they're kept and raised once it returns
        if (!finish_head(t)) return 0;
        (*t->on_data)(std::string_view(data, size * n));
        return size * n;
    } catch (...) {
        t->error = std::current_exception();
        return 0;
    }
}

Response perform(const std::string& url, const std::string& method, const std::string& body, const Headers& headers,
                 long timeout, bool raise_status, const std::function<bool(const Response&)>* on_head,
                 const std::function<void(std::string_view)>& on_data) {
    ensure_curl();
    CURL* curl = curl_easy_init();
    if (!curl) throw OSError("libcurl couldn't start");
    Transfer t;
    t.curl = curl;
    t.raise_status = raise_status;
    t.on_head = on_head;
    t.on_data = &on_data;
    char errors[CURL_ERROR_SIZE] = "";
    curl_slist* list = nullptr;
    for (const auto& h : headers) list = curl_slist_append(list, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);  // give up after `timeout` seconds without data
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, timeout);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errors);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    } else if (method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK && !t.error && !t.head_done) {  // a response with no body
        try {
            finish_head(&t);
        } catch (...) {
            t.error = std::current_exception();
        }
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    if (t.error) std::rethrow_exception(t.error);
    if (rc != CURLE_OK && !(t.stopped && rc == CURLE_WRITE_ERROR))
        throw OSError(errors[0] ? errors : curl_easy_strerror(rc));
    return t.head;
}

}  // namespace

Response request(const std::string& url, const std::string& method, const std::string& body, const Headers& headers,
                 long timeout) {
    std::string received;
    Response r = perform(url, method, body, headers, timeout, false, nullptr,
                         [&](std::string_view chunk) { received.append(chunk); });
    r.body = std::move(received);
    return r;
}

Response urlopen(const std::string& url, const std::string& method, const std::string& body, const Headers& headers,
                 long timeout) {
    Response r = request(url, method, body, headers, timeout);
    if (r.status >= 400) throw HttpError(r.status, r.reason, r.body);
    return r;
}

Response stream(const std::string& url, const Headers& headers, long timeout,
                const std::function<bool(const Response&)>& on_head,
                const std::function<void(std::string_view)>& on_data) {
    return perform(url, "GET", "", headers, timeout, true, &on_head, on_data);
}

std::string unquote_plus(std::string_view s) {
    auto hex = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
            out += static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

Dict<std::vector<std::string>> parse_qs(std::string_view query) {
    Dict<std::vector<std::string>> out;
    for (const auto& pair : py::split(query, "&")) {
        size_t eq = pair.find('=');
        if (pair.empty() || eq == std::string::npos) continue;
        std::string value = unquote_plus(pair.substr(eq + 1));
        if (!value.empty()) out[unquote_plus(pair.substr(0, eq))].push_back(value);
    }
    return out;
}

// ---- the server side ----

namespace {

const char* reason_for(int code) {
    switch (code) {
        case 100: return "Continue";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 413: return "Request Entity Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default: return "";
    }
}

bool write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = ::send(fd, data.data(), data.size(), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

}  // namespace

Connection::~Connection() { ::close(fd_); }

bool Connection::fill() {
    char chunk[65536];
    while (true) {
        ssize_t n = ::recv(fd_, chunk, sizeof chunk, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        buf_.append(chunk, static_cast<size_t>(n));
        return true;
    }
}

bool Connection::read_head() {
    size_t end = std::string::npos, skip = 0;
    while (true) {
        size_t crlf = buf_.find("\r\n\r\n"), lf = buf_.find("\n\n");
        if (crlf != std::string::npos && (lf == std::string::npos || crlf < lf)) end = crlf, skip = 4;
        else if (lf != std::string::npos) end = lf, skip = 2;
        if (end != std::string::npos) break;
        if (buf_.size() > 65536 || !fill()) return false;
    }
    auto lines = py::split(std::string_view(buf_).substr(0, end), "\n");
    pos_ = end + skip;
    auto request_line = py::split(py::rstrip(lines[0], "\r"));
    if (request_line.size() != 3) return false;
    method = request_line[0];
    target = request_line[1];
    version = request_line[2];
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string line = py::rstrip(lines[i], "\r");
        size_t colon = line.find(':');
        if (colon != std::string::npos) headers_.emplace_back(py::lower(line.substr(0, colon)), py::strip(line.substr(colon + 1)));
    }
    return true;
}

std::string Connection::header(std::string_view name) const {
    std::string key = py::lower(name);
    for (const auto& [k, v] : headers_)
        if (k == key) return v;
    return "";
}

size_t Connection::read(char* out, size_t n) {
    if (pos_ < buf_.size()) {
        size_t take = std::min(n, buf_.size() - pos_);
        std::memcpy(out, buf_.data() + pos_, take);
        pos_ += take;
        return take;
    }
    if (!continued_ && py::lower(header("Expect")) == "100-continue") {
        write_all(fd_, "HTTP/1.1 100 Continue\r\n\r\n");
    }
    continued_ = true;
    while (true) {
        ssize_t got = ::recv(fd_, out, n, 0);
        if (got < 0 && errno == EINTR) continue;
        return got > 0 ? static_cast<size_t>(got) : 0;
    }
}

std::string Connection::read_body(size_t n) {
    std::string out(n, '\0');
    size_t have = 0;
    while (have < n) {
        size_t got = read(out.data() + have, n - have);
        if (got == 0) break;
        have += got;
    }
    out.resize(have);
    return out;
}

void Connection::send(int code, std::string_view body, std::string_view content_type,
                      const std::vector<std::pair<std::string, std::string>>& headers) {
    char date[64];
    std::time_t t = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&t, &utc);
    std::strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", &utc);
    std::string head = py::format("HTTP/1.0 %d %s\r\nServer: SportsRulesCouncil\r\nDate: %s\r\n", code, reason_for(code), date);
    head += "Content-Type: " + std::string(content_type) + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& [name, value] : headers) head += name + ": " + value + "\r\n";
    head += "\r\n";
    if (write_all(fd_, head)) write_all(fd_, body);
}

Server::Server(const std::string& host, int port, Handler handler)
    : handler_(std::make_shared<Handler>(std::move(handler))) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) throw OSError(std::strerror(errno));
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || ::listen(fd_, 16) != 0) {
        int err = errno;
        ::close(fd_);
        fd_ = -1;
        throw OSError(py::format("[Errno %d] %s", err, std::strerror(err)));
    }
    socklen_t len = sizeof addr;
    getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
}

Server::~Server() {
    shutdown();
    if (fd_ >= 0) ::close(fd_);
}

void Server::start() { thread_ = std::thread([this] { loop(); }); }

void Server::shutdown() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void Server::loop() {
    while (!stop_) {
        pollfd p{fd_, POLLIN, 0};
        if (poll(&p, 1, 500) <= 0) continue;  // wake up twice a second to see if it's time to stop
        int client = ::accept(fd_, nullptr, nullptr);
        if (client < 0) continue;
#ifdef SO_NOSIGPIPE
        int one = 1;  // a client that hangs up mid-answer shouldn't kill the server with SIGPIPE
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        timeval wait{60, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
        std::thread([handler = handler_, client] {
            try {
                Connection conn(client);
                if (conn.read_head()) (*handler)(conn);
            } catch (...) {  // a failed request mustn't take the server down with it
            }
        }).detach();
    }
}

}  // namespace http
