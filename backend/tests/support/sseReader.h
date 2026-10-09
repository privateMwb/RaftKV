#pragma once

// A minimal SSE client for tests: opens GET <path> on 127.0.0.1:port, parses the
// response head, then yields events one at a time as they arrive. It reads the
// raw socket on purpose: FalconHTTP's HttpClient waits for a response to finish,
// which a stream never does.

// clang-format off
#include <FalconHTTP/Core/Socket.h> // FalconHTTP::Core::Socket - raw TCP

#include <chrono>   // std::chrono
#include <cstdint>  // uint16_t
#include <map>      // std::map - response headers
#include <optional> // std::optional
#include <string>   // std::string
// clang-format on

class SseReader {
  public:
    struct Event {
        std::string name; ///< The `event:` field ("message" if absent).
        std::string data; ///< The `data:` field.
    };

    SseReader(uint16_t port, const std::string& path) : socket_(FalconHTTP::Core::Socket::createTcp()) {
        using Ms = std::chrono::milliseconds;
        if (!socket_.isValid() || !socket_.connect("127.0.0.1", port, Ms(1000)))
            return;

        const std::string request = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                    "Accept: text/event-stream\r\nConnection: keep-alive\r\n\r\n";
        if (socket_.send(request.data(), request.size()) <= 0)
            return;

        // Read until the blank line that ends the response head.
        (void)socket_.setReceiveTimeout(Ms(3000));
        while (buffer_.find("\r\n\r\n") == std::string::npos) {
            if (!fill())
                return;
        }

        const std::size_t headEnd = buffer_.find("\r\n\r\n");
        parseHead(buffer_.substr(0, headEnd));
        buffer_.erase(0, headEnd + 4);
        open_ = true;
    }

    /// @return true if the connection was made and a response head arrived.
    [[nodiscard]] bool connected() const {
        return open_;
    }

    /// @return The HTTP status of the response head (0 if none).
    [[nodiscard]] int status() const {
        return status_;
    }

    /// @return A response header value (name lowercase), or "" if absent.
    [[nodiscard]] std::string header(const std::string& name) const {
        const auto found = headers_.find(name);
        return found == headers_.end() ? std::string() : found->second;
    }

    /// @brief Waits up to @p timeout for the next complete event.
    /// @return The event, or `std::nullopt` on timeout, EOF, or error (see closed()).
    std::optional<Event> next(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (true) {
            if (auto event = takeEvent())
                return event;

            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0 || !socket_.isValid())
                return std::nullopt;

            (void)socket_.setReceiveTimeout(left < std::chrono::milliseconds(20) ? std::chrono::milliseconds(20) : left);
            if (!fill())
                return std::nullopt;
        }
    }

    /// @return true once the server has closed the stream (EOF seen).
    [[nodiscard]] bool closed() const {
        return eof_;
    }

    void close() {
        socket_.close();
    }

  private:
    // Appends whatever arrives next. false on timeout, EOF, or error.
    bool fill() {
        char chunk[2048];
        const std::ptrdiff_t got = socket_.receive(chunk, sizeof(chunk));
        if (got == 0)
            eof_ = true;
        if (got <= 0)
            return false;

        buffer_.append(chunk, static_cast<std::size_t>(got));
        return true;
    }

    void parseHead(const std::string& head) {
        const std::size_t firstSpace = head.find(' ');
        if (firstSpace != std::string::npos)
            status_ = std::atoi(head.c_str() + firstSpace + 1);

        std::size_t pos = head.find("\r\n");
        while (pos != std::string::npos) {
            const std::size_t start = pos + 2;
            const std::size_t end = head.find("\r\n", start);
            const std::string line = head.substr(start, end == std::string::npos ? end : end - start);
            const std::size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                for (char& c : name)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                std::size_t valueStart = colon + 1;
                while (valueStart < line.size() && line[valueStart] == ' ')
                    ++valueStart;
                headers_[name] = line.substr(valueStart);
            }
            pos = end;
        }
    }

    // Extracts one "\n\n"-terminated event from the buffer, if complete.
    std::optional<Event> takeEvent() {
        const std::size_t end = buffer_.find("\n\n");
        if (end == std::string::npos)
            return std::nullopt;

        const std::string block = buffer_.substr(0, end);
        buffer_.erase(0, end + 2);

        Event event;
        event.name = "message";
        std::size_t pos = 0;
        while (pos <= block.size()) {
            const std::size_t eol = block.find('\n', pos);
            const std::string line = block.substr(pos, eol == std::string::npos ? eol : eol - pos);
            if (line.rfind("event: ", 0) == 0)
                event.name = line.substr(7);
            else if (line.rfind("data: ", 0) == 0)
                event.data = line.substr(6);
            else if (line == "data:")
                event.data.clear();
            if (eol == std::string::npos)
                break;
            pos = eol + 1;
        }
        return event;
    }

    FalconHTTP::Core::Socket socket_;
    std::string buffer_;
    std::map<std::string, std::string> headers_;
    int status_ = 0;
    bool open_ = false;
    bool eof_ = false;
};
