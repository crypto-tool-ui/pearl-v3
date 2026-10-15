#include "cp_pool.h"
#include "cp_e2e.h"
#include "cp_config.h"
#include "cp_job_ctrl.h"
#include "cp_platform.h"
#include "cp_state.h"
#include "cp_util.h"
#include "cp_proof.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static cp_sock_t tcp_sock = CP_INVALID_SOCK;
static std::atomic<double> g_diff{32.0};
static std::atomic<int> g_submit_inflight{0};
static std::atomic<int> g_net_reader_run{0};
static std::atomic<int> g_net_conn_lost{0};
static std::mutex g_net_mx;
static std::mutex g_pending_mx;
static CpPendingJob g_pending_job;
static int g_pending_valid = 0;
static std::thread g_net_reader;
static std::deque<std::string> g_pool_inbox;
static std::mutex g_inbox_mx;
static std::condition_variable g_inbox_cv;
static char net_buf[65536];
static int net_pos = 0;
static char json_msg[65536];
static int g_e2e_mode = 0;
static void* g_e2e_session = NULL;
static constexpr size_t E2E_FRAME_MAX = 1024 * 1024 + 64;
static char legacy_wire_buf[2 * E2E_FRAME_MAX];
static size_t legacy_wire_len = 0;

namespace {
std::string json_escape(const char *value)
{
    std::string out;
    for (const unsigned char c : std::string(value ? value : "")) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back((char)c); }
        else if (c < 0x20) return {};
        else out.push_back((char)c);
    }
    return out;
}

} // namespace

static int socket_recv(cp_sock_t sock, void* out, size_t size)
{
    size_t got = 0;
    char* bytes = (char*)out;
    while(got < size){
#ifdef _WIN32
        int n = recv((SOCKET)sock, bytes + got, (int)(size - got), 0);
#else
        ssize_t n = recv(sock, bytes + got, size - got, 0);
#endif
        if(n <= 0) return 0;
        got += (size_t)n;
    }
    return 1;
}

static int legacy_read_line(cp_sock_t sock, char* out, size_t cap)
{
    for(;;){
        char* end = (char*)memchr(legacy_wire_buf, '\n', legacy_wire_len);
        if(end){
            size_t len = (size_t)(end - legacy_wire_buf);
            if(len >= cap) return 0;
            memcpy(out, legacy_wire_buf, len);
            out[len] = 0;
            const size_t tail = legacy_wire_len - len - 1;
            memmove(legacy_wire_buf, end + 1, tail);
            legacy_wire_len = tail;
            return 1;
        }
        if(legacy_wire_len == sizeof(legacy_wire_buf)) return 0;
#ifdef _WIN32
        int n = recv((SOCKET)sock, legacy_wire_buf + legacy_wire_len,
                     (int)(sizeof(legacy_wire_buf) - legacy_wire_len), 0);
#else
        ssize_t n = recv(sock, legacy_wire_buf + legacy_wire_len,
                         sizeof(legacy_wire_buf) - legacy_wire_len, 0);
#endif
        if(n <= 0) return 0;
        legacy_wire_len += (size_t)n;
    }
}

static int hex_decode_line(const char* input, std::vector<uint8_t>& out)
{
    const size_t len = strlen(input);
    if(len % 2) return 0;
    out.resize(len / 2);
    for(size_t i = 0; i < len; i += 2){
        auto nibble = [](char c) -> int {
            if(c >= '0' && c <= '9') return c - '0';
            if(c >= 'a' && c <= 'f') return c - 'a' + 10;
            if(c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nibble(input[i]), lo = nibble(input[i + 1]);
        if(hi < 0 || lo < 0) return 0;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

static std::string hex_encode(const uint8_t* bytes, size_t len)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out(len * 2, '0');
    for(size_t i = 0; i < len; ++i){
        out[2 * i] = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 15];
    }
    return out;
}

static int pool_send_json(const char* json)
{
    if(!g_e2e_mode) return cp_send_json(tcp_sock, json);
    std::string plain(json ? json : "");
    if(plain.empty()) return 0;
    // The legacy tunnel encrypts a line without its delimiter; PRL7 transports raw stream bytes.
    if(g_e2e_mode != 1) plain.push_back('\n');
    std::vector<uint8_t> encrypted(E2E_FRAME_MAX);
    size_t encrypted_len = 0;
    uint8_t err[256] = {0};
    if(cp_e2e_encrypt(g_e2e_session, (const uint8_t*)plain.data(), plain.size(),
                      encrypted.data(), encrypted.size(), &encrypted_len,
                      err, sizeof(err)) != 0){
        fprintf(stderr, "[proxy] E2E encryption failed: %s\n", err);
        return 0;
    }
    if(g_e2e_mode == 1){
        const std::string line = hex_encode(encrypted.data(), encrypted_len) + "\n";
        return cp_send_all(tcp_sock, line.data(), line.size());
    }
    return cp_send_all(tcp_sock, encrypted.data(), encrypted_len);
}

static int read_plain_chunk(cp_sock_t sock)
{
    if(!g_e2e_mode){
#ifdef _WIN32
        int n = recv((SOCKET)sock, net_buf + net_pos, (int)sizeof(net_buf) - net_pos - 1, 0);
#else
        ssize_t n = recv(sock, net_buf + net_pos, sizeof(net_buf) - net_pos - 1, 0);
#endif
        if(n <= 0) return 0;
        net_pos += (int)n;
        return 1;
    }

    std::vector<uint8_t> frame;
    if(g_e2e_mode == 1){
        std::vector<char> line(E2E_FRAME_MAX * 2 + 1);
        if(!legacy_read_line(sock, line.data(), line.size())) return 0;
        if(!hex_decode_line(line.data(), frame)) return 0;
    } else {
        uint8_t prefix[4];
        if(!socket_recv(sock, prefix, sizeof(prefix))) return 0;
        const uint32_t size = (uint32_t(prefix[0]) << 24) | (uint32_t(prefix[1]) << 16) |
                              (uint32_t(prefix[2]) << 8) | uint32_t(prefix[3]);
        if(size > E2E_FRAME_MAX || size < 1) return 0;
        frame.resize(size + 4);
        memcpy(frame.data(), prefix, 4);
        if(!socket_recv(sock, frame.data() + 4, size)) return 0;
    }
    std::vector<uint8_t> plain(E2E_FRAME_MAX);
    size_t plain_len = 0;
    uint8_t err[256] = {0};
    if(cp_e2e_decrypt(g_e2e_session, frame.data(), frame.size(), plain.data(), plain.size(),
                      &plain_len, err, sizeof(err)) != 0){
        fprintf(stderr, "[proxy] E2E decryption failed: %s\n", err);
        return 0;
    }
    if(plain_len > sizeof(net_buf) - net_pos - 1) return 0;
    memcpy(net_buf + net_pos, plain.data(), plain_len);
    net_pos += (int)plain_len;
    return 1;
}

static cp_sock_t tcp_connect(const char* host, int port)
{
    if(cp_net_init() != 0){
        fprintf(stderr, "WSAStartup failed\n");
        return CP_INVALID_SOCK;
    }
    struct hostent* he = gethostbyname(host);
    if(!he){ perror("gethostbyname"); return CP_INVALID_SOCK; }
    cp_sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if(s == CP_INVALID_SOCK){ perror("socket"); return CP_INVALID_SOCK; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    if(connect(s, (struct sockaddr*)&sa, sizeof(sa)) < 0){
        perror("connect");
        CP_SOCK_CLOSE(s);
        return CP_INVALID_SOCK;
    }

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    int keepalive = 1;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive));
#ifdef _WIN32
    DWORD rcv_to = 10000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_to, sizeof(rcv_to));
#else
    struct timeval rcv_tv;
    rcv_tv.tv_sec = 10;
    rcv_tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_tv, sizeof(rcv_tv));
#endif
#ifdef _WIN32
    struct tcp_keepalive kalive;
    kalive.onoff = 1;
    kalive.keepalivetime = 15000;    /* 15s idle probe */
    kalive.keepaliveinterval = 3000; /* 3s probe interval */
    DWORD ret_bytes = 0;
    WSAIoctl(s, SIO_KEEPALIVE_VALS, &kalive, sizeof(kalive), NULL, 0, &ret_bytes, NULL, NULL);
#elif defined(__linux__)
    int idle = 15, intvl = 3, cnt = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif

    return s;
}

static void queue_pending_job(
    const char* job_id, const char* job_key,
    const uint8_t* header, const char* target_hex, const uint32_t tgt[8],
    uint32_t cert_version)
{
    std::lock_guard<std::mutex> lk(g_pending_mx);
    strncpy(g_pending_job.job_id, job_id, sizeof(g_pending_job.job_id) - 1);
    g_pending_job.job_id[sizeof(g_pending_job.job_id) - 1] = 0;
    strncpy(g_pending_job.job_key, job_key, sizeof(g_pending_job.job_key) - 1);
    g_pending_job.job_key[sizeof(g_pending_job.job_key) - 1] = 0;
    strncpy(g_pending_job.target_hex, target_hex, sizeof(g_pending_job.target_hex) - 1);
    g_pending_job.target_hex[sizeof(g_pending_job.target_hex) - 1] = 0;
    memcpy(g_pending_job.header, header, INCOMPLETE_HEADER_BYTES);
    memcpy(g_pending_job.tgt, tgt, 8 * sizeof(uint32_t));
    g_pending_job.cert_version = cp_resolve_cert_version(cert_version);
    g_pending_valid = 1;
}

static void pool_inbox_push(const char* line)
{
    std::lock_guard<std::mutex> lk(g_inbox_mx);
    g_pool_inbox.emplace_back(line);
    g_inbox_cv.notify_one();
}

static int net_wait_readable(cp_sock_t sock, int timeout_ms)
{
    if(sock == CP_INVALID_SOCK) return 0;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#ifdef _WIN32
    return select(0, &fds, NULL, NULL, &tv) > 0;
#else
    return select(sock + 1, &fds, NULL, NULL, &tv) > 0;
#endif
}

static int net_buf_has_complete_json(void)
{
    char* start = (char*)memchr(net_buf, '{', net_pos);
    if(!start) return 0;
    int depth = 0;
    for(char* p = start; p < net_buf + net_pos; p++){
        if(*p == '{') depth++;
        else if(*p == '}'){
            depth--;
            if(depth == 0) return 1;
        }
    }
    return 0;
}

static char* pop_json_message(cp_sock_t sock)
{
    while(1){
        char* start = (char*)memchr(net_buf, '{', net_pos);
        if(!start){
            if(net_pos >= (int)sizeof(net_buf) - 1) net_pos = 0;
            if(!read_plain_chunk(sock)) return NULL;
            continue;
        }
        int depth = 0;
        char* end = start;
        for(; end < net_buf + net_pos; end++){
            if(*end == '{') depth++;
            else if(*end == '}'){
                depth--;
                if(depth == 0) break;
            }
        }
        if(depth != 0 || end >= net_buf + net_pos){
            if(net_pos >= (int)sizeof(net_buf) - 1) net_pos = 0;
            if(!read_plain_chunk(sock)) return NULL;
            continue;
        }
        int len = (int)(end - start) + 1;
        if(len >= (int)sizeof(json_msg)) len = (int)sizeof(json_msg) - 1;
        memcpy(json_msg, start, (size_t)len);
        json_msg[len] = 0;
        int tail = (int)(net_buf + net_pos - (end + 1));
        memmove(net_buf, end + 1, (size_t)tail);
        net_pos = tail;
        return json_msg;
    }
}

static void pool_dispatch_line(const char* line)
{
    if(strstr(line, "mining.set_difficulty")){
        double d = cp_json_num(line, "params");
        if(!d){
            const char* p = strstr(line, "\"params\"");
            if(p){
                p = strchr(p, '[');
                if(p){
                    p++;
                    while(*p == ' ' || *p == '\t') p++;
                    d = atof(p);
                }
            }
        }
        if(d > 0.0){
            g_diff.store(d);
            printf("[DDP] Loss scale updated: %.0f%s\n", g_diff.load(),
                   cp_job_mining_active() ? " (active step)" : "");
            fflush(stdout);
        }
        return;
    }

    if(strstr(line, "result") || strstr(line, "error")){
        if(g_submit_inflight.load())
            printf("[DDP] Gradient aggregation response: %s\n", line);
        else
            printf("[DDP] Coordinator response: %s\n", line);
        fflush(stdout);
        g_submit_inflight.store(0);
        return;
    }

    if(strstr(line, "mining.notify")){
        char job_id[128] = {0};
        char header_hex[320] = {0};
        char target_hex[80] = {0};
        uint32_t cert_version = 0;
        if(!cp_pool_parse_notify(line, job_id, sizeof(job_id),
                                header_hex, sizeof(header_hex),
                                target_hex, sizeof(target_hex),
                                &cert_version)){
            return;
        }
        cert_version = cp_resolve_cert_version(cert_version);

        char job_key[320];
        snprintf(job_key, sizeof(job_key), "%s:%.16s", job_id, header_hex);

        uint8_t header[INCOMPLETE_HEADER_BYTES];
        int hlen = cp_hex_to_bytes(header_hex, header, INCOMPLETE_HEADER_BYTES);
        if(hlen != INCOMPLETE_HEADER_BYTES) return;

        uint32_t tgt[8];
        memset(tgt, 0, sizeof(tgt));
        if(target_hex[0]) {
            if(!cp_be_target_hex_to_le_words(target_hex, tgt)) return;
        } else if(strstr(line, "\"header\"")) {
            // Kryptex object notifications require an explicit pool target.
            return;
        } else {
            cp_target_from_difficulty(g_diff.load(), tgt);
        }

        if(cp_job_mining_active()){
            if(cp_job_key_matches(job_key)) return;
            cp_job_request_cancel();
            queue_pending_job(job_id, job_key, header, target_hex, tgt, cert_version);
            printf("[DDP] New gradient target batch=%s (superseding batch=%s)\n",
                   job_id, cp_job_mining_key());
            fflush(stdout);
            return;
        }

        pool_inbox_push(line);
        return;
    }

    if(!cp_job_mining_active())
        pool_inbox_push(line);
    else
        printf("[DDP] (during step) %s\n", line);
    fflush(stdout);
}

static void pool_net_reader_thread(void)
{
    while(g_net_reader_run.load()){
        /* Drain buffered messages before waiting — pool often sends authorize
         * ack + mining.notify back-to-back in one TCP segment. */
        if(!net_buf_has_complete_json()){
            if(!net_wait_readable(tcp_sock, 100)) continue;
        }
        std::lock_guard<std::mutex> lk(g_net_mx);
        char* line = pop_json_message(tcp_sock);
        if(!line){
            g_net_conn_lost.store(1);
            g_inbox_cv.notify_all();
            cp_job_request_cancel();
            printf("[DDP] Coordinator connection lost (stream terminated)\n"); fflush(stdout);
            return;
        }
        // suppress raw stratum messages in logs
        pool_dispatch_line(line);
    }
}

int cp_pool_connect(const char* host, int port, int e2e_mode)
{
    tcp_sock = tcp_connect(host, port);
    if(tcp_sock == CP_INVALID_SOCK) return 0;
    g_e2e_mode = e2e_mode;
    legacy_wire_len = 0;
    net_pos = 0;
    if(!g_e2e_mode) return 1;

    uint8_t hello[256] = {0}, error[256] = {0};
    size_t hello_len = 0;
    void* handshake = NULL;
    if(cp_e2e_start(g_e2e_mode, hello, sizeof(hello), &hello_len, &handshake,
                    error, sizeof(error)) != 0 ||
       !cp_send_all(tcp_sock, hello, hello_len)){
        fprintf(stderr, "[proxy] E2E client hello failed: %s\n", error);
        cp_e2e_abort_handshake(handshake);
        cp_pool_disconnect();
        return 0;
    }

    std::vector<uint8_t> server_hello;
    if(g_e2e_mode == 1){
        char line[128];
        if(!legacy_read_line(tcp_sock, line, sizeof(line))){
            cp_e2e_abort_handshake(handshake);
            cp_pool_disconnect();
            return 0;
        }
        server_hello.assign(line, line + strlen(line));
    } else {
        uint8_t prefix[4];
        if(!socket_recv(tcp_sock, prefix, sizeof(prefix))){
            cp_e2e_abort_handshake(handshake);
            cp_pool_disconnect();
            return 0;
        }
        const uint32_t size = (uint32_t(prefix[0]) << 24) | (uint32_t(prefix[1]) << 16) |
                              (uint32_t(prefix[2]) << 8) | uint32_t(prefix[3]);
        if(size > 4096 || size == 0){
            cp_e2e_abort_handshake(handshake);
            cp_pool_disconnect();
            return 0;
        }
        server_hello.resize(size + 4);
        memcpy(server_hello.data(), prefix, 4);
        if(!socket_recv(tcp_sock, server_hello.data() + 4, size)){
            cp_e2e_abort_handshake(handshake);
            cp_pool_disconnect();
            return 0;
        }
    }
    if(cp_e2e_finish(handshake, server_hello.data(), server_hello.size(),
                     &g_e2e_session, error, sizeof(error)) != 0){
        fprintf(stderr, "[proxy] E2E handshake failed: %s\n", error);
        cp_pool_disconnect();
        return 0;
    }
    if(g_e2e_mode == 1) legacy_wire_len = 0;
    printf("[proxy] E2E transport established (mode=%s)\n",
           g_e2e_mode == 1 ? "legacy tunnel" : "PRL7");
    return 1;
}

void cp_pool_disconnect(void)
{
    if(g_e2e_session){ cp_e2e_destroy(g_e2e_session); g_e2e_session = NULL; }
    g_e2e_mode = 0;
    legacy_wire_len = 0;
    if(tcp_sock != CP_INVALID_SOCK){
        CP_SOCK_CLOSE(tcp_sock);
        tcp_sock = CP_INVALID_SOCK;
    }
    net_pos = 0;
}

cp_sock_t cp_pool_socket(void)
{
    return tcp_sock;
}

int cp_pool_send_authorize(int msg_id, const char* wallet,
                           const char* worker, const char* agent,
                           const char* password)
{
    const std::string w = json_escape((std::string(wallet ? wallet : "") + "." +
                                       (worker ? worker : "")).c_str());
    const std::string a = json_escape(agent), p = json_escape(password);
    if (w.empty() || a.empty() || p.empty()) return 0;
    const std::string msg = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(msg_id) +
        ",\"method\":\"mining.authorize\",\"params\":{\"wallet\":\"" + w +
        "\",\"agent\":\"" + a + "\",\"password\":\"" + p + "\",\"type\":\"v2\"}}";
    printf("[Kryptex] Sending v2 authorization\n"); fflush(stdout);
    return pool_send_json(msg.c_str());
}

int cp_pool_send_plain_proof_submit(cp_sock_t sock, int msg_id, const char* job_id,
                                    const char* plain_b64, double hs)
{
    if(!job_id || !plain_b64) return 0;
    std::string job = json_escape(job_id);
    std::vector<char> proof_b64(PLAIN_PROOF_B64_MAX);
    char error[256] = {0};
    if (job.empty() || cp_proof_kryptex_v2_encode(
            (const uint8_t*)plain_b64, strlen(plain_b64), proof_b64.data(),
            proof_b64.size(), error, sizeof(error)) != 0) {
        fprintf(stderr, "[Kryptex] failed to encode verified proof for v2 submit: %s\n", error);
        return 0;
    }
    const std::string proof(proof_b64.data());
    if (proof.empty()) {
        fprintf(stderr, "[Kryptex] failed to encode verified proof for v2 submit\n");
        return 0;
    }
    const std::string encoded = json_escape(proof.c_str());
    const std::string msg = "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(msg_id) +
        ",\"method\":\"mining.submit\",\"params\":{\"hs\":" +
        std::to_string((uint64_t)(hs < 0 ? 0 : hs)) + ",\"job_id\":\"" + job +
        "\",\"plain_proof\":\"" + encoded + "\"}}";
    printf("[Kryptex] Submitting verified share job=%s proof=%zu bytes\n", job_id, proof.size());
    fflush(stdout);
    if(sock != tcp_sock) return 0;
    return pool_send_json(msg.c_str());
}

void cp_pool_reader_start(void)
{
    if(g_net_reader_run.load()) return;
    if(g_net_reader.joinable()) g_net_reader.join();
    g_net_conn_lost.store(0);
    g_net_reader_run.store(1);
    g_net_reader = std::thread(pool_net_reader_thread);
    printf("[DDP] NCCL asynchronous telemetry stream listener initialized\n");
    fflush(stdout);
}

void cp_pool_reader_stop(void)
{
    if(!g_net_reader_run.load() && !g_net_reader.joinable()) return;
    g_net_reader_run.store(0);
    g_inbox_cv.notify_all();
    if(tcp_sock != CP_INVALID_SOCK){
#ifdef _WIN32
        shutdown((SOCKET)tcp_sock, SD_BOTH);
#else
        shutdown(tcp_sock, SHUT_RDWR);
#endif
    }
    if(g_net_reader.joinable()) g_net_reader.join();
}

void cp_pool_inbox_clear(void)
{
    std::scoped_lock lk(g_inbox_mx, g_pending_mx);
    g_pool_inbox.clear();
    g_pending_valid = 0;
}

int cp_pool_wait_line(char* out, size_t out_cap, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(g_inbox_mx);
    for(;;){
        if(!g_pool_inbox.empty()){
            strncpy(out, g_pool_inbox.front().c_str(), out_cap - 1);
            out[out_cap - 1] = 0;
            g_pool_inbox.pop_front();
            return 1;
        }
        if(g_net_conn_lost.load() || !g_net_reader_run.load()) return -1;
        if(timeout_ms < 0){
            g_inbox_cv.wait(lk);
            continue;
        }
        if(g_inbox_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms))
           == std::cv_status::timeout){
            return 0;
        }
    }
}

int cp_pool_conn_lost(void)
{
    return g_net_conn_lost.load();
}

void cp_pool_set_submit_inflight(int on)
{
    g_submit_inflight.store(on);
}

void cp_pool_log_share_submit_outcome(void)
{
    if(g_submit_inflight.load())
        printf("[DDP] Gradient update sent; awaiting master node acknowledgment\n");
    else
        printf("[DDP] Gradient update acknowledged by master node\n");
    fflush(stdout);
}

int cp_pool_parse_notify(const char* json,
                         char* job_id, int job_len,
                         char* header_hex, int header_len,
                         char* target_hex, int target_len,
                         uint32_t* cert_version_out)
{
    job_id[0] = header_hex[0] = target_hex[0] = 0;
    uint32_t cert_version = 0;
    double cv = cp_json_num(json, "cert_version");
    if(cv >= 1.0 && cv <= 3.0)
        cert_version = (uint32_t)cv;
    if(cert_version_out)
        *cert_version_out = cert_version;

    if(strstr(json, "\"header\"")){
        cp_json_str(json, "job_id", job_id, job_len);
        cp_json_str(json, "header", header_hex, header_len);
        cp_json_str(json, "target", target_hex, target_len);
        if(target_hex[0]) {
            char* value = target_hex;
            if(value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
                memmove(value, value + 2, strlen(value + 2) + 1);
            size_t len = strlen(value);
            if(len == 0 || len > TARGET_HEX_LEN) return 0;
            char padded[TARGET_HEX_LEN + 1];
            memset(padded, '0', TARGET_HEX_LEN - len);
            memcpy(padded + TARGET_HEX_LEN - len, value, len + 1);
            memcpy(target_hex, padded, sizeof(padded));
        }
        return header_hex[0] != 0;
    }

    const char* p = strstr(json, "\"params\"");
    if(!p) return 0;
    p = strchr(p, '[');
    if(!p) return 0;
    p++;
    while(*p && *p != ']'){
        while(*p==' ' || *p=='\t' || *p==',') p++;
        if(*p == '"'){
            p++;
            char tmp[320];
            int i = 0;
            while(*p && *p != '"' && i < (int)sizeof(tmp) - 1) tmp[i++] = *p++;
            tmp[i] = 0;
            if(i == 0){ if(*p=='"') p++; continue; }
            if(!job_id[0]){
                strncpy(job_id, tmp, job_len - 1);
                job_id[job_len - 1] = 0;
            } else if((int)strlen(tmp) == TARGET_HEX_LEN && !target_hex[0]){
                strncpy(target_hex, tmp, target_len - 1);
                target_hex[target_len - 1] = 0;
            } else if((int)strlen(tmp) == HEADER_HEX_LEN && !header_hex[0]){
                strncpy(header_hex, tmp, header_len - 1);
                header_hex[header_len - 1] = 0;
            }
            if(*p=='"') p++;
        } else if(*p=='{' ){
            break;
        } else {
            while(*p && *p != ',' && *p != ']') p++;
        }
    }
    return header_hex[0] != 0;
}

int cp_pool_take_pending_job(CpPendingJob* out)
{
    std::lock_guard<std::mutex> lk(g_pending_mx);
    if(!g_pending_valid) return 0;
    *out = g_pending_job;
    g_pending_valid = 0;
    return 1;
}

double cp_pool_difficulty(void)
{
    return g_diff.load();
}

void cp_pool_set_difficulty(double d)
{
    g_diff.store(d);
}
