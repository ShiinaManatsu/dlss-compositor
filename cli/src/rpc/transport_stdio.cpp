// =============================================================================
// dlss/rpc — JSON-RPC 2.0 over stdio transport
//
// Frames messages with LSP-style "Content-Length: N\r\n\r\n<body>" headers.
//
// Critical Windows note: stdin/stdout MUST be set to _O_BINARY before any
// I/O — text mode would translate \n -> \r\n on writes and strip \r on
// reads, corrupting both Content-Length framing and body byte counts.
// =============================================================================
#include "dispatcher.h"

#include "../core/log.h"

#include <cstddef>
#include <iostream>
#include <ostream>
#include <string>

#include <nlohmann/json.hpp>

#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
#endif

namespace dlss {

namespace {

/// Read one JSON-RPC message from stdin (Content-Length framed, LSP-style).
/// Returns empty string on EOF or framing error.
std::string readJsonRpcMessage() {
    std::size_t contentLength = 0;
    std::string line;

    // Header phase: parse `Content-Length: N` until a bare CRLF.
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;  // empty line = end of headers

        const std::string prefix = "Content-Length:";
        if (line.compare(0, prefix.size(), prefix) == 0) {
            std::string value = line.substr(prefix.size());
            // Trim leading whitespace (header may be "Content-Length:N" or ": N").
            std::size_t start = value.find_first_not_of(" \t");
            if (start != std::string::npos) value = value.substr(start);
            try {
                contentLength = std::stoull(value);
            } catch (const std::exception&) {
                return {};
            }
        }
        // Other headers (Content-Type, etc.) are ignored.
    }

    if (contentLength == 0) return {};

    std::string body(contentLength, '\0');
    if (!std::cin.read(body.data(), static_cast<std::streamsize>(contentLength))) {
        return {};
    }
    return body;
}

/// Write one JSON-RPC response to stdout (Content-Length framed).
void writeJsonRpcResponse(const nlohmann::json& response) {
    const std::string body = response.dump();
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::cout.flush();
}

} // namespace

/// Run the JSON-RPC stdio server loop.
/// Reads framed messages from stdin, dispatches via RpcDispatcher, writes
/// framed responses to stdout. Terminates on stdin EOF.
void runJsonRpcStdio(RpcDispatcher& dispatcher) {
#ifdef _WIN32
    // CRITICAL: binary mode prevents CRLF translation on stdin/stdout, which
    // would otherwise corrupt framing and the byte-count of the body.
    _setmode(_fileno(stdin),  _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    LOG_INFO("[JSON-RPC] stdio binary mode configured");
    LOG_INFO("[JSON-RPC] stdio server started");

    while (std::cin.good() && !std::cin.eof()) {
        std::string body = readJsonRpcMessage();
        if (body.empty()) {
            if (std::cin.eof()) break;
            continue;
        }

        nlohmann::json request;
        try {
            LOG_DEBUG("[JSON-RPC] request bytes={}", body.size());
            request = nlohmann::json::parse(body);
        } catch (const nlohmann::json::exception& e) {
            // fmt v11 requires lvalue refs in make_format_args; copy first.
            const std::string what = e.what();
            LOG_WARN("[JSON-RPC] parse error: {}", what);
            writeJsonRpcResponse({
                {"jsonrpc", "2.0"},
                {"id",      nullptr},
                {"error",   {{"code", -32700}, {"message", "Parse error"}}},
            });
            continue;
        }

        const std::string method = request.value("method", std::string{});
        const nlohmann::json id = request.contains("id") ? request["id"] : nlohmann::json(nullptr);
        const nlohmann::json params = request.value("params", nlohmann::json::object());
        LOG_DEBUG("[JSON-RPC] dispatch begin method={}", method);

        nlohmann::json result = dispatcher.dispatch(method, params);
        LOG_DEBUG("[JSON-RPC] dispatch complete method={}", method);

        writeJsonRpcResponse({
            {"jsonrpc", "2.0"},
            {"id",      id},
            {"result",  result},
        });
        LOG_DEBUG("[JSON-RPC] response written method={}", method);
    }

    LOG_INFO("[JSON-RPC] stdio server stopped");
}

} // namespace dlss
