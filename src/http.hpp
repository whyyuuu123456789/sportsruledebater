// HTTP both ways: a client (libcurl) for the models and for downloading clips, and a small threaded server for the
// website. The server answers one request per connection, as Python's http.server does.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "util.hpp"

namespace http {

// An error status from the server, like urllib's HTTPError: "HTTP Error 404: Not Found".
struct HttpError : OSError {
    HttpError(int code, const std::string& reason, std::string body = "");
    int code;
    std::string body;
};

struct Response {
    int status = 0;
    std::string reason;
    std::string content_type = "text/plain";  // the media type, lowercase, without parameters
    std::string url;                           // where it ended up, after redirects
    std::string body;
};

using Headers = std::vector<std::string>;  // "Name: value"

// One request. An error status comes back in the response; OSError means the server couldn't be reached. timeout is
// in seconds, for connecting and for each wait for data.
Response request(const std::string& url, const std::string& method = "GET", const std::string& body = "",
                 const Headers& headers = {}, long timeout = 30);
// Like urlopen: an error status raises HttpError.
Response urlopen(const std::string& url, const std::string& method = "GET", const std::string& body = "",
                 const Headers& headers = {}, long timeout = 30);
// A GET that hands over the body as it arrives. on_head sees the final response's status and headers first; returning
// false stops there. An error status raises HttpError.
Response stream(const std::string& url, const Headers& headers, long timeout,
                const std::function<bool(const Response&)>& on_head,
                const std::function<void(std::string_view)>& on_data);

std::string unquote_plus(std::string_view s);
// urllib.parse.parse_qs: each name's values, with blank values left out.
Dict<std::vector<std::string>> parse_qs(std::string_view query);

// One request to the server, read from its socket.
class Connection {
public:
    explicit Connection(int fd) : fd_(fd) {}
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    bool read_head();  // the request line and headers; false if there isn't a well-formed request
    std::string method, target, version;
    std::string header(std::string_view name) const;  // "" if it wasn't sent
    size_t read(char* out, size_t n);                 // up to n bytes of the body; 0 at its end
    std::string read_body(size_t n);                  // n bytes, or fewer if the client stops sending
    void send(int code, std::string_view body, std::string_view content_type,
              const std::vector<std::pair<std::string, std::string>>& headers = {});

private:
    bool fill();
    int fd_;
    std::string buf_;
    size_t pos_ = 0;
    bool continued_ = false;
    std::vector<std::pair<std::string, std::string>> headers_;
};

class Server {
public:
    using Handler = std::function<void(Connection&)>;
    Server(const std::string& host, int port, Handler handler);  // binds; port 0 picks a free one
    ~Server();
    int port() const { return port_; }
    void start();     // serves in a background thread, one thread per request
    void shutdown();  // stops accepting and waits for the serving thread

private:
    void loop();
    int fd_ = -1;
    int port_ = 0;
    std::shared_ptr<Handler> handler_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};

}  // namespace http
