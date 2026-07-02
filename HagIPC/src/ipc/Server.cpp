#include "PCH.h"
#include "ipc/Server.h"
#include "ipc/MemAccess.h"
#include "ipc/CallExec.h"
#include "Offsets.h"
#include "Log.h"

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <sstream>
#include <vector>
#include <charconv>
#include <cstdlib>

#pragma comment(lib, "Ws2_32.lib")

namespace hag::ipc {

namespace {

// ---- parsing helpers ----
std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

bool ParseU64(const std::string& s, std::uint64_t& out) {
    int base = 10;
    std::size_t i = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; i = 2; }
    if (i >= s.size()) return false;
    const char* b = s.data() + i;
    const char* e = s.data() + s.size();
    auto r = std::from_chars(b, e, out, base);
    return r.ec == std::errc() && r.ptr == e;
}

struct TypeInfo { std::size_t size; char kind; };  // kind: 'u','i','f','p'
bool ResolveType(const std::string& t, TypeInfo& ti) {
    if (t == "u8")  { ti = {1, 'u'}; return true; }
    if (t == "u16") { ti = {2, 'u'}; return true; }
    if (t == "u32") { ti = {4, 'u'}; return true; }
    if (t == "u64") { ti = {8, 'u'}; return true; }
    if (t == "i8")  { ti = {1, 'i'}; return true; }
    if (t == "i16") { ti = {2, 'i'}; return true; }
    if (t == "i32") { ti = {4, 'i'}; return true; }
    if (t == "i64") { ti = {8, 'i'}; return true; }
    if (t == "f32") { ti = {4, 'f'}; return true; }
    if (t == "f64") { ti = {8, 'f'}; return true; }
    if (t == "ptr") { ti = {8, 'p'}; return true; }
    return false;
}

// Resolve an offset token to a runtime address: a file address off 0x140000000 (default, e.g.
// 0x141976838), or a raw runtime VA if prefixed "abs:".
bool ParseAddr(const std::string& s, std::uintptr_t& addr, std::string& err) {
    std::string t = s;
    bool isAbs = false;
    if (t.rfind("abs:", 0) == 0) { isAbs = true; t = t.substr(4); }
    std::uint64_t v = 0;
    if (!ParseU64(t, v)) { err = "bad offset"; return false; }
    addr = isAbs ? static_cast<std::uintptr_t>(v) : offsets::FromRVA(static_cast<std::uintptr_t>(v));
    return true;
}

// off (tk[1]) + optional chain (from firstChainIdx) -> (start, chain[]) for MemAccess.
bool BuildChain(const std::vector<std::string>& tk, std::size_t firstChainIdx,
                std::uintptr_t& start, std::vector<std::uintptr_t>& chain, std::string& err) {
    if (tk.size() < 2 || !ParseAddr(tk[1], start, err)) { if (err.empty()) err = "bad offset"; return false; }
    for (std::size_t i = firstChainIdx; i < tk.size(); ++i) {
        std::uint64_t c = 0;
        if (!ParseU64(tk[i], c)) { err = "bad chain offset"; return false; }
        chain.push_back(static_cast<std::uintptr_t>(c));
    }
    return true;
}

// Parse a value for 'write' per type into raw bits[ti.size].
bool ParseValue(const TypeInfo& ti, const std::string& s, std::uint8_t out[8], std::string& err) {
    if (ti.kind == 'f') {
        char* end = nullptr;
        double d = std::strtod(s.c_str(), &end);
        if (end == s.c_str()) { err = "bad float value"; return false; }
        if (ti.size == 4) { float f = static_cast<float>(d); std::memcpy(out, &f, 4); }
        else              { std::memcpy(out, &d, 8); }
        return true;
    }
    std::uint64_t v = 0;
    if (!ParseU64(s, v)) {
        char* end = nullptr;
        long long sv = std::strtoll(s.c_str(), &end, 10);
        if (end == s.c_str() || *end != '\0') { err = "bad int value"; return false; }
        v = static_cast<std::uint64_t>(sv);
    }
    std::memcpy(out, &v, ti.size);
    return true;
}

bool HexDecode(const std::string& s, std::vector<std::uint8_t>& out) {
    if (s.empty() || (s.size() & 1)) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < s.size(); i += 2) {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::string Hex(std::uint64_t v) {
    std::ostringstream o; o << "0x" << std::hex << v; return o.str();
}

}  // namespace

Server& Server::Get() { static Server s; return s; }

void Server::Start(std::uint16_t port, std::string token) {
    if (m_running.exchange(true)) return;
    m_port = port;
    m_token = std::move(token);
    m_thread = std::thread([this] { Run(); });
}

void Server::Stop() {
    m_running = false;
    if (m_thread.joinable()) m_thread.detach();  // blocked on accept(); let the process tear it down
}

void Server::Run() {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { HAG_LOG("WSAStartup failed"); return; }

    SOCKET listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) { HAG_LOG("socket() failed"); ::WSACleanup(); return; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(m_port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);   // loopback ONLY

    if (::bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        ::listen(listenSock, 1) == SOCKET_ERROR) {
        HAG_LOG("bind/listen failed on 127.0.0.1 (WSA err " + std::to_string(::WSAGetLastError()) + ")");
        ::closesocket(listenSock); ::WSACleanup(); return;
    }
    HAG_LOG("HagIPC listening on 127.0.0.1:" + std::to_string(m_port));

    while (m_running) {
        SOCKET client = ::accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) { if (!m_running) break; continue; }
        Serve(static_cast<std::uintptr_t>(client));
        ::closesocket(client);
    }
    ::closesocket(listenSock);
    ::WSACleanup();
}

void Server::Serve(std::uintptr_t clientSock) {
    const SOCKET sock = static_cast<SOCKET>(clientSock);
    bool authed = m_token.empty();
    std::string buf;
    char recvBuf[2048];

    auto sendLine = [&](const std::string& s) {
        std::string line = s; line += '\n';
        ::send(sock, line.c_str(), static_cast<int>(line.size()), 0);
    };

    sendLine(authed ? "ok HagIPC ready (send 'help')" : "ok HagIPC ready (auth required: auth <token>)");

    while (m_running) {
        const int n = ::recv(sock, recvBuf, sizeof(recvBuf), 0);
        if (n <= 0) break;
        buf.append(recvBuf, recvBuf + n);

        std::size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            if (!authed) {
                auto tk = Split(line);
                if (tk.size() == 2 && tk[0] == "auth" && tk[1] == m_token) { authed = true; sendLine("ok authed"); }
                else sendLine("err auth required");
                continue;
            }
            sendLine(Dispatch(line));
        }
    }
}

std::string Server::Dispatch(const std::string& line) {
    const auto tk = Split(line);
    if (tk.empty()) return "err empty";
    const std::string& cmd = tk[0];

    if (cmd == "ping") return "ok pong";
    if (cmd == "base") return "ok " + Hex(offsets::Base());
    if (cmd == "help")
        return "ok cmds: ping | base | read <off> <type> [chain..] | readb <off> <len> [chain..] | "
               "write <off> <type> <val> [chain..] | call <off> [a0..a7] | exec <hexblob>  "
               "(off = file address off 0x140000000, e.g. 0x141976838, or abs:<VA>; "
               "type = u8/u16/u32/u64/i*/f32/f64/ptr; chain: each extra offset does p=*p+c; "
               "call/exec args are 64-bit ints; all commands run inline on the socket thread so they "
               "work from the front-end menu)";

    if (cmd == "read") {
        if (tk.size() < 3) return "err usage: read <off> <type> [chain..]";
        TypeInfo ti{};
        if (!ResolveType(tk[2], ti)) return "err bad type";
        std::uintptr_t start = 0; std::vector<std::uintptr_t> chain; std::string err;
        if (!BuildChain(tk, 3, start, chain, err)) return "err " + err;

        std::uint8_t raw[8] = {};
        std::uintptr_t finalAddr = 0;
        if (!mem::ReadChain(start, chain.data(), chain.size(), raw, ti.size, &finalAddr))
            return "err access violation (bad offset/chain)";

        std::ostringstream o;
        if (ti.kind == 'f') {
            if (ti.size == 4) { float f; std::memcpy(&f, raw, 4); o << f; }
            else              { double d; std::memcpy(&d, raw, 8); o << d; }
            o << " @" << Hex(finalAddr);
        } else if (ti.kind == 'i') {
            std::int64_t v = 0;
            std::memcpy(&v, raw, ti.size);
            const int shift = (8 - static_cast<int>(ti.size)) * 8;
            v = (v << shift) >> shift;   // sign-extend from ti.size
            o << v << " " << Hex(static_cast<std::uint64_t>(v)) << " @" << Hex(finalAddr);
        } else {  // 'u' or 'p'
            std::uint64_t v = 0;
            std::memcpy(&v, raw, ti.size);
            o << v << " " << Hex(v) << " @" << Hex(finalAddr);
        }
        return "ok " + o.str();
    }

    if (cmd == "readb") {
        if (tk.size() < 3) return "err usage: readb <off> <len> [chain..]";
        std::uint64_t len = 0;
        if (!ParseU64(tk[2], len) || len == 0 || len > 4096) return "err bad len (1..4096)";
        std::uintptr_t start = 0; std::vector<std::uintptr_t> chain; std::string err;
        if (!BuildChain(tk, 3, start, chain, err)) return "err " + err;

        std::vector<std::uint8_t> data(static_cast<std::size_t>(len));
        std::uintptr_t finalAddr = 0;
        if (!mem::ReadChain(start, chain.data(), chain.size(), data.data(), data.size(), &finalAddr))
            return "err access violation (bad offset/chain)";

        std::ostringstream o;
        o << "@" << Hex(finalAddr) << " ";
        for (auto b : data) { static const char* H = "0123456789abcdef"; o << H[b >> 4] << H[b & 0xF]; }
        return "ok " + o.str();
    }

    if (cmd == "write") {
        if (tk.size() < 4) return "err usage: write <off> <type> <val> [chain..]";
        TypeInfo ti{};
        if (!ResolveType(tk[2], ti)) return "err bad type";
        std::uint8_t bits[8] = {};
        std::string verr;
        if (!ParseValue(ti, tk[3], bits, verr)) return "err " + verr;
        std::uintptr_t start = 0; std::vector<std::uintptr_t> chain; std::string cerr;
        if (!BuildChain(tk, 4, start, chain, cerr)) return "err " + cerr;

        std::uintptr_t fa = 0;
        if (!mem::WriteChain(start, chain.data(), chain.size(), bits, ti.size, &fa))
            return "err access violation (bad offset/chain)";
        return "ok @" + Hex(fa);
    }

    if (cmd == "call") {
        if (tk.size() < 2) return "err usage: call <off> [a0 a1 .. up to 8]";
        std::uintptr_t addr = 0; std::string aerr;
        if (!ParseAddr(tk[1], addr, aerr)) return "err " + aerr;
        if (tk.size() - 2 > 8) return "err too many args (max 8)";
        std::vector<std::uint64_t> args;
        for (std::size_t i = 2; i < tk.size(); ++i) {
            std::uint64_t v = 0;
            if (!ParseU64(tk[i], v)) {
                char* end = nullptr; long long sv = std::strtoll(tk[i].c_str(), &end, 10);
                if (end == tk[i].c_str() || *end != '\0') return "err bad arg (64-bit ints only)";
                v = static_cast<std::uint64_t>(sv);
            }
            args.push_back(v);
        }
        std::uint64_t rax = 0;
        if (!exec::Call(addr, args.data(), static_cast<int>(args.size()), rax))
            return "err call faulted (seh)";
        return "ok " + Hex(rax);
    }

    if (cmd == "exec") {
        if (tk.size() != 2) return "err usage: exec <hexblob>  (machine code, entry@0, ends with ret)";
        std::vector<std::uint8_t> code;
        if (!HexDecode(tk[1], code) || code.size() > 65536) return "err bad hex blob (even-length hex, <=64KiB)";
        std::uint64_t rax = 0;
        if (!exec::ExecBlob(code.data(), code.size(), rax))
            return "err exec faulted (seh)";
        return "ok " + Hex(rax);
    }

    return "err unknown command (try 'help')";
}

}  // namespace hag::ipc
