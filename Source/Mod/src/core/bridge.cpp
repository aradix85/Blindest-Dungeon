
// ---- THE CLAUDE BRIDGE ----
// Fork-only, compiled in only with /DAX_BRIDGE (build.ps1 -Bridge). A loopback-only HTTP/1.1
// server (raw Winsock, one thread per connection) for an assistant playing along with a blind
// player:
//   GET  /health                      build, context, mute state, sequence counters
//   GET  /speech?since=N&wait=MS&limit=K   lines handed to the screen reader (full text)
//   GET  /log?since=N&grep=TEXT&wait=MS&limit=K   the debug-log stream (file on or off)
//   GET  /context                     active mod context + the last focus (nav) line
//   POST /input?gap=MS  body: keys    e.g. "down down enter", "shift+d", "wait:500", "f10"
//   GET  /input                       queue state;  POST /input?clear=1 empties the queue
//   POST /mute  body: on|off          silence the reader (lines are still captured)
// Without `since` a read returns the newest `limit` lines. Every read starts with "next=<seq>":
// pass that back as `since` to get only what came after.
//
// Threading: the HTTP threads only touch the bridge's own buffers (each behind its own lock).
// Keys are pressed by bridgePump() on the game's main thread (OurPoll), through axRoutePadKey --
// the same path the controller translation uses, so a key behaves exactly like the player's.
// Env: DDACCESS_BRIDGE_PORT (default 8773), DDACCESS_NO_BRIDGE=1, DDACCESS_MUTE=1 (start muted).
// The launcher starts the game through Steam, so env vars rarely arrive: the file
// %LOCALAPPDATA%\DarkestAccess\bridge-mute also starts the session muted.
#ifdef AX_BRIDGE

#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <deque>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include "internal.h"

namespace {

// ---- Ring buffers ----
struct BrLine { unsigned long seq; DWORD tick; std::string text; };

struct BrRing {
    CRITICAL_SECTION cs;
    std::deque<BrLine> q;
    unsigned long next;
    size_t cap;
    explicit BrRing(size_t c) : next(1), cap(c) { InitializeCriticalSection(&cs); }

    void push(const char* text) {
        std::string s(text ? text : "");
        for (size_t i = 0; i < s.size(); i++)
            if (s[i] == '\r' || s[i] == '\n' || s[i] == '\t') s[i] = ' ';
        EnterCriticalSection(&cs);
        BrLine l; l.seq = next++; l.tick = GetTickCount(); l.text.swap(s);
        q.push_back(std::move(l));
        while (q.size() > cap) q.pop_front();
        LeaveCriticalSection(&cs);
    }

    unsigned long nextSeq() {
        EnterCriticalSection(&cs);
        unsigned long n = next;
        LeaveCriticalSection(&cs);
        return n;
    }

    // since > 0: the first `limit` matching lines with seq >= since, "next=" continues after them.
    // since == 0: the newest `limit` matching lines, "next=" is the ring's next sequence number.
    std::string dump(unsigned long since, const std::string& grepLower, size_t limit) {
        std::string out;
        EnterCriticalSection(&cs);
        unsigned long first = q.empty() ? next : q.front().seq;
        std::deque<const BrLine*> hit;
        unsigned long resume = next;
        for (size_t i = 0; i < q.size(); i++) {
            const BrLine& l = q[i];
            if (since && l.seq < since) continue;
            if (!grepLower.empty()) {
                std::string low(l.text);
                for (size_t k = 0; k < low.size(); k++) low[k] = (char)tolower((unsigned char)low[k]);
                if (low.find(grepLower) == std::string::npos) continue;
            }
            hit.push_back(&l);
            if (since) {
                if (hit.size() >= limit) { resume = l.seq + 1; break; }
            } else if (hit.size() > limit) {
                hit.pop_front();
            }
        }
        char head[96];
        _snprintf(head, sizeof head, "next=%lu first=%lu\n", resume, first);
        head[sizeof head - 1] = 0;
        out += head;
        for (size_t i = 0; i < hit.size(); i++) {
            char pre[48];
            _snprintf(pre, sizeof pre, "%lu\t%lu\t", hit[i]->seq, (unsigned long)hit[i]->tick);
            pre[sizeof pre - 1] = 0;
            out += pre;
            out += hit[i]->text;
            out += '\n';
        }
        LeaveCriticalSection(&cs);
        return out;
    }
};

BrRing& speechRing() { static BrRing r(1000); return r; }
BrRing& logRing()    { static BrRing r(8000); return r; }

// ---- Shared state: the last focus line and the key queue ----
struct BrKey { uint32_t sym; uint16_t mod; DWORD delay; std::string name; };

struct BrShared {
    CRITICAL_SECTION cs;
    std::string lastNav;
    DWORD lastNavTick;
    std::deque<BrKey> keys;
    DWORD gap;
    unsigned long keysSent;
    BrShared() : lastNavTick(0), gap(150), keysSent(0) { InitializeCriticalSection(&cs); }
};
BrShared& shared() { static BrShared s; return s; }

volatile LONG g_brMuted = -1;                  // -1 = not read from the environment yet
const char* volatile g_brContext = "none";     // always a string literal from the context table
int g_brPort = 0;

// ---- Key names ----
bool parseKey(const std::string& tokIn, BrKey* out) {
    std::string tok;
    for (size_t i = 0; i < tokIn.size(); i++) tok += (char)tolower((unsigned char)tokIn[i]);
    out->sym = 0; out->mod = 0; out->delay = 0; out->name = tokIn;
    if (tok.compare(0, 5, "wait:") == 0) {
        long ms = atol(tok.c_str() + 5);
        if (ms < 0) ms = 0;
        if (ms > 30000) ms = 30000;
        out->delay = (DWORD)ms;
        return true;
    }
    std::string key = tok;
    for (;;) {                                  // leading modifiers: shift+ctrl+alt+...
        size_t p = key.find('+');
        if (p == std::string::npos || p == 0 || p == key.size() - 1) break;
        std::string m = key.substr(0, p);
        if (m == "shift")                        out->mod |= KMOD_LSHIFT;
        else if (m == "ctrl" || m == "control") out->mod |= KMOD_LCTRL;
        else if (m == "alt")                    out->mod |= KMOD_LALT;
        else return false;
        key = key.substr(p + 1);
    }
    struct Named { const char* n; uint32_t sym; };
    static const Named kNamed[] = {
        { "enter", SDLK_RETURN }, { "return", SDLK_RETURN },
        { "esc", SDLK_ESCAPE }, { "escape", SDLK_ESCAPE },
        { "tab", SDLK_TAB }, { "space", SDLK_SPACE }, { "backspace", SDLK_BACKSPACE },
        { "delete", SDLK_DELETE }, { "del", SDLK_DELETE },
        { "up", SDLK_UP }, { "down", SDLK_DOWN }, { "left", SDLK_LEFT }, { "right", SDLK_RIGHT },
        { "home", SDLK_HOME }, { "end", SDLK_END },
        { "pageup", SDLK_PAGEUP }, { "pgup", SDLK_PAGEUP },
        { "pagedown", SDLK_PAGEDOWN }, { "pgdn", SDLK_PAGEDOWN },
        { "comma", SDLK_COMMA }, { "period", SDLK_PERIOD }, { "dot", SDLK_PERIOD },
        { "backquote", SDLK_BACKQUOTE }, { "grave", SDLK_BACKQUOTE }, { "tilde", SDLK_BACKQUOTE },
        { "slash", SDLK_SLASH }, { "backslash", SDLK_BACKSLASH },
        { "minus", SDLK_MINUS }, { "equals", SDLK_EQUALS },
    };
    for (size_t i = 0; i < sizeof kNamed / sizeof kNamed[0]; i++)
        if (key == kNamed[i].n) { out->sym = kNamed[i].sym; return true; }
    if (key.size() >= 2 && key.size() <= 3 && key[0] == 'f') {
        bool digits = true;
        for (size_t i = 1; i < key.size(); i++) if (!isdigit((unsigned char)key[i])) digits = false;
        int n = digits ? atoi(key.c_str() + 1) : 0;
        if (n >= 1 && n <= 12) { out->sym = 0x4000003Au + (uint32_t)(n - 1); return true; }
    }
    if (key.size() == 1) {
        unsigned char c = (unsigned char)key[0];
        if (c > 0x20 && c < 0x7f) { out->sym = c; return true; }
    }
    return false;
}

// ---- Tiny HTTP helpers ----
std::string brUrlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
                 isxdigit((unsigned char)s[i + 2])) {
            char hx[3] = { s[i + 1], s[i + 2], 0 };
            o += (char)strtol(hx, nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

bool queryParam(const std::string& query, const char* key, std::string* out) {
    size_t klen = strlen(key);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string part = query.substr(pos, amp - pos);
        if (part.size() >= klen && part.compare(0, klen, key) == 0 &&
            (part.size() == klen || part[klen] == '=')) {
            *out = part.size() > klen ? brUrlDecode(part.substr(klen + 1)) : std::string();
            return true;
        }
        pos = amp + 1;
    }
    return false;
}

unsigned long paramUL(const std::string& query, const char* key, unsigned long def) {
    std::string v;
    if (!queryParam(query, key, &v) || v.empty()) return def;
    return strtoul(v.c_str(), nullptr, 10);
}

std::string brTrim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string brFmt(const char* f, ...) {
    char buf[2048];
    va_list ap; va_start(ap, f);
    _vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    return buf;
}

const char* kHelp =
    "Blindest Dungeon bridge (fork, debug build)\n"
    "GET  /health\n"
    "GET  /speech?since=N&wait=MS&limit=K\n"
    "GET  /log?since=N&grep=TEXT&wait=MS&limit=K\n"
    "GET  /context\n"
    "POST /input?gap=MS   body: keys, e.g. \"down down enter\", \"shift+d\", \"wait:500\"\n"
    "GET  /input          POST /input?clear=1\n"
    "POST /mute           body: on|off\n";

// ---- Routes ----
int route(const std::string& method, const std::string& target, const std::string& body,
          std::string* resp) {
    std::string path = target, query;
    size_t qm = target.find('?');
    if (qm != std::string::npos) { path = target.substr(0, qm); query = target.substr(qm + 1); }

    if (path == "/" || path == "/help") { *resp = kHelp; return 200; }

    if (path == "/health") {
        BrShared& s = shared();
        EnterCriticalSection(&s.cs);
        size_t pending = s.keys.size();
        unsigned long sent = s.keysSent;
        LeaveCriticalSection(&s.cs);
        *resp = brFmt("ok\nbuild=%s %s\ngame=%s\nhooked=%d\ncontext=%s\nmuted=%d\nreader=%s\n"
                    "speech_next=%lu\nlog_next=%lu\nlogfile=%d\nkeys_pending=%u\nkeys_sent=%lu\nport=%d\n",
                    __DATE__, __TIME__, axGameBuildName(), g_enabled ? 1 : 0, g_brContext,
                    bridgeMuted() ? 1 : 0,
                    axSpeechReaderName() ? axSpeechReaderName() : "none",
                    speechRing().nextSeq(), logRing().nextSeq(), axDebugLogEnabled() ? 1 : 0,
                    (unsigned)pending, sent, g_brPort);
        return 200;
    }

    if (path == "/speech" || path == "/log") {
        if (method != "GET") { *resp = "use GET\n"; return 405; }
        bool isLog = path == "/log";
        BrRing& r = isLog ? logRing() : speechRing();
        unsigned long since = paramUL(query, "since", 0);
        unsigned long waitMs = paramUL(query, "wait", 0);
        unsigned long limit = paramUL(query, "limit", isLog ? 200 : 50);
        if (limit < 1) limit = 1;
        if (limit > 5000) limit = 5000;
        if (waitMs > 30000) waitMs = 30000;
        std::string grep;
        queryParam(query, "grep", &grep);
        for (size_t k = 0; k < grep.size(); k++) grep[k] = (char)tolower((unsigned char)grep[k]);
        if (since && waitMs) {
            DWORD start = GetTickCount();
            while (r.nextSeq() <= since && GetTickCount() - start < waitMs) Sleep(40);
        }
        *resp = r.dump(since, grep, (size_t)limit);
        return 200;
    }

    if (path == "/context" || path == "/focus") {
        BrShared& s = shared();
        EnterCriticalSection(&s.cs);
        std::string nav = s.lastNav;
        DWORD navTick = s.lastNavTick;
        LeaveCriticalSection(&s.cs);
        *resp = brFmt("context=%s\nfocus_age_ms=%lu\nfocus=", g_brContext,
                    navTick ? (unsigned long)(GetTickCount() - navTick) : 0ul);
        *resp += nav;
        *resp += '\n';
        return 200;
    }

    if (path == "/input") {
        BrShared& s = shared();
        if (method == "POST") {
            std::string clear;
            if (queryParam(query, "clear", &clear)) {
                EnterCriticalSection(&s.cs);
                size_t n = s.keys.size();
                s.keys.clear();
                LeaveCriticalSection(&s.cs);
                *resp = brFmt("cleared=%u\n", (unsigned)n);
                return 200;
            }
            std::deque<BrKey> add;
            std::string tok;
            std::string b = body + " ";
            for (size_t i = 0; i < b.size(); i++) {
                if (isspace((unsigned char)b[i])) {
                    if (!tok.empty()) {
                        BrKey k;
                        if (!parseKey(tok, &k)) { *resp = "unknown key: " + tok + "\n"; return 400; }
                        add.push_back(k);
                        tok.clear();
                    }
                } else tok += b[i];
            }
            unsigned long gap = paramUL(query, "gap", 0);
            EnterCriticalSection(&s.cs);
            if (gap) s.gap = gap < 30 ? 30 : gap > 5000 ? 5000 : (DWORD)gap;
            for (size_t i = 0; i < add.size(); i++) s.keys.push_back(add[i]);
            size_t pending = s.keys.size();
            DWORD g = s.gap;
            LeaveCriticalSection(&s.cs);
            *resp = brFmt("queued=%u\npending=%u\ngap=%lu\nspeech_next=%lu\n", (unsigned)add.size(),
                        (unsigned)pending, (unsigned long)g, speechRing().nextSeq());
            return 200;
        }
        EnterCriticalSection(&s.cs);
        *resp = brFmt("pending=%u\nsent=%lu\ngap=%lu\n", (unsigned)s.keys.size(), s.keysSent,
                    (unsigned long)s.gap);
        LeaveCriticalSection(&s.cs);
        return 200;
    }

    if (path == "/mute") {
        if (method == "POST") {
            std::string v = brTrim(body);
            for (size_t k = 0; k < v.size(); k++) v[k] = (char)tolower((unsigned char)v[k]);
            if (v != "on" && v != "off") { *resp = "body must be on or off\n"; return 400; }
            bridgeMuted();                                   // settle the env default first
            InterlockedExchange(&g_brMuted, v == "on" ? 1 : 0);
            logLine("bridge: speech %s", v == "on" ? "MUTED" : "unmuted");
            if (v == "on") g_silenceReq = true;              // cut whatever is being spoken now
        }
        *resp = brFmt("muted=%d\n", bridgeMuted() ? 1 : 0);
        return 200;
    }

    *resp = "not found\n" + std::string(kHelp);
    return 404;
}

// ---- One connection ----
bool sendAll(SOCKET c, const char* p, int n) {
    while (n > 0) {
        int w = send(c, p, n, 0);
        if (w <= 0) return false;
        p += w; n -= w;
    }
    return true;
}

void serveOne(SOCKET c) {
    std::string data;
    char buf[8192];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) return;
        data.append(buf, (size_t)n);
        headerEnd = data.find("\r\n\r\n");
        if (data.size() > (1u << 20)) return;
    }
    std::string header = data.substr(0, headerEnd);
    size_t lineEnd = header.find("\r\n");
    std::string requestLine = header.substr(0, lineEnd);
    size_t sp1 = requestLine.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
    std::string method = sp1 == std::string::npos ? requestLine : requestLine.substr(0, sp1);
    std::string target = sp1 == std::string::npos ? "/"
                       : requestLine.substr(sp1 + 1, (sp2 == std::string::npos ? requestLine.size() : sp2) - sp1 - 1);

    size_t contentLength = 0;
    {
        std::string low(header);
        for (size_t k = 0; k < low.size(); k++) low[k] = (char)tolower((unsigned char)low[k]);
        size_t cl = low.find("\r\ncontent-length:");
        if (cl != std::string::npos) contentLength = strtoul(low.c_str() + cl + 17, nullptr, 10);
        if (contentLength > (1u << 20)) contentLength = 1u << 20;
    }
    size_t bodyStart = headerEnd + 4;
    while (data.size() - bodyStart < contentLength) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        data.append(buf, (size_t)n);
    }
    std::string body = data.size() > bodyStart
                     ? data.substr(bodyStart, contentLength < data.size() - bodyStart ? contentLength
                                                                                       : data.size() - bodyStart)
                     : std::string();

    std::string resp;
    int status = 500;
    try {
        status = route(method, target, body, &resp);
    } catch (...) {
        status = 500;
        resp = "handler error\n";
    }
    const char* reason = status == 200 ? "OK" : status == 400 ? "Bad Request"
                       : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" : "Error";
    std::string head = brFmt("HTTP/1.1 %d %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                           "Content-Length: %u\r\nConnection: close\r\n\r\n",
                           status, reason, (unsigned)resp.size());
    if (sendAll(c, head.data(), (int)head.size())) sendAll(c, resp.data(), (int)resp.size());
}

DWORD WINAPI connThread(LPVOID p) {
    SOCKET c = (SOCKET)(uintptr_t)p;
    DWORD to = 15000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof to);
    serveOne(c);
    shutdown(c, SD_SEND);
    closesocket(c);
    return 0;
}

DWORD WINAPI serverThread(LPVOID) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { logLine("bridge: WSAStartup failed -- bridge off"); return 0; }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { logLine("bridge: socket() failed (%d) -- bridge off", WSAGetLastError()); return 0; }
    BOOL excl = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_brPort);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);              // loopback only, never the network
    if (bind(ls, (sockaddr*)&a, sizeof a) == SOCKET_ERROR || listen(ls, 16) == SOCKET_ERROR) {
        logLine("bridge: cannot listen on 127.0.0.1:%d (error %d) -- bridge off", g_brPort, WSAGetLastError());
        closesocket(ls);
        return 0;
    }
    logLine("bridge: listening on http://127.0.0.1:%d (muted=%d)", g_brPort, bridgeMuted() ? 1 : 0);
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) { Sleep(100); continue; }
        HANDLE h = CreateThread(nullptr, 0, connThread, (LPVOID)(uintptr_t)c, 0, nullptr);
        if (h) CloseHandle(h);
        else closesocket(c);
    }
}

bool muteFlagFile() {
    const char* local = getenv("LOCALAPPDATA");
    if (!local || !*local) return false;
    char path[MAX_PATH];
    _snprintf(path, sizeof path, "%s\\DarkestAccess\\bridge-mute", local);
    path[sizeof path - 1] = 0;
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

// ---- Public hooks ----
bool bridgeMuted() {
    LONG m = g_brMuted;
    if (m < 0) {
        const char* e = getenv("DDACCESS_MUTE");
        InterlockedCompareExchange(&g_brMuted, ((e && *e == '1') || muteFlagFile()) ? 1 : 0, -1);
        m = g_brMuted;
    }
    return m == 1;
}

void bridgeNoteSpeech(const char* text, bool interrupt) {
    (void)interrupt;
    speechRing().push(text);
}

void bridgeNotePosted(const char* text, int kind) {
    if (kind != SPK_NAV || !text) return;
    BrShared& s = shared();
    EnterCriticalSection(&s.cs);
    s.lastNav = text;
    s.lastNavTick = GetTickCount();
    LeaveCriticalSection(&s.cs);
}

void bridgeNoteLog(const char* line) {
    logRing().push(line);
}

void bridgeNoteContext(const char* name) {
    g_brContext = name ? name : "none";
}

void bridgeStart() {
    static bool started = false;
    if (started) return;
    started = true;
    const char* off = getenv("DDACCESS_NO_BRIDGE");
    if (off && *off == '1') { logLine("bridge: disabled by DDACCESS_NO_BRIDGE=1"); return; }
    g_brPort = 8773;
    const char* p = getenv("DDACCESS_BRIDGE_PORT");
    if (p && atoi(p) > 0 && atoi(p) < 65536) g_brPort = atoi(p);
    bridgeMuted();
    HANDLE h = CreateThread(nullptr, 0, serverThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

// ---- The key pump (main thread) ----
void bridgePump(uintptr_t base) {
    static bool  upPending = false;
    static BrKey up;
    static DWORD nextAt = 0;
    DWORD now = GetTickCount();

    if (upPending) {                                    // the release half, one frame later
        axRoutePadKey(base, SDL_EVT_KEYUP, up.sym, up.mod, 0, false);
        upPending = false;
        return;
    }
    if (nextAt && (long)(now - nextAt) < 0) return;
    if (synthKeyQueued() > 0) return;                   // the game has not taken the last one yet

    BrShared& s = shared();
    BrKey k;
    bool have = false;
    DWORD gap = 150;
    EnterCriticalSection(&s.cs);
    if (!s.keys.empty()) { k = s.keys.front(); s.keys.pop_front(); have = true; }
    gap = s.gap;
    LeaveCriticalSection(&s.cs);
    if (!have) { nextAt = 0; return; }

    if (!k.sym) {                                       // wait:MS
        nextAt = now + k.delay;
        logLine("bridge: wait %lu ms", (unsigned long)k.delay);
        return;
    }
    g_inputIsController = false;                        // keyboard wording, as for a real key
    bool claimed = axRoutePadKey(base, SDL_EVT_KEYDOWN, k.sym, k.mod, 0, true);
    logLine("bridge: key \"%s\" sym=0x%x mod=0x%x -> %s", k.name.c_str(), k.sym, (unsigned)k.mod,
            claimed ? "claimed by the mod" : "passed to the game");
    upPending = true;
    up = k;
    nextAt = now + gap;
    EnterCriticalSection(&s.cs);
    s.keysSent++;
    LeaveCriticalSection(&s.cs);
}

#endif  // AX_BRIDGE
