/* sink.c -- .wav recording and raw-PCM streaming, both drained by the UI
 * thread from the audio thread's ring buffer.
 *
 * THE POINT OF THE RING
 *
 * The audio thread cannot write to a file. fwrite() can block on a spinning
 * disk, on a full page cache, on an encrypted filesystem doing work; send()
 * can block on a TCP window that a laptop on wifi has stopped opening. Any
 * of those inside the audio thread is a missed deadline and an audible gap.
 *
 * So the audio thread does the one thing it can do in bounded time: it
 * writes int16s into a preallocated array and bumps an index. Everything
 * that can be slow happens over here, on a thread that is allowed to be
 * slow. If this thread is late, the ring absorbs it (11 seconds of slack at
 * 96kHz). If this thread is catastrophically late, the ring laps and we drop
 * the oldest samples and count them -- a hole in the recording, but the
 * instrument never stutters.
 */

#include "bytebeat.h"
#include "sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

/* ---- wav ---- */
static FILE     *g_wav;
static unsigned  g_wav_frames;
static int       g_wav_rate;
static char      g_wav_path[256];

/* ---- network ----
 *
 * A freshly accepted client is in PROBE: we do not know yet whether it is a
 * browser (which sends "GET / HTTP/1.1" immediately) or a raw listener like
 * `nc` or `ffplay tcp://...` (which sends nothing at all and just waits).
 *
 * So we wait a few service ticks. If a request arrives we answer with HTTP
 * headers and a WAV header and the browser plays it with no arguments and no
 * software installed. If nothing arrives, it is a raw listener and we stream
 * bare PCM exactly as before. */
enum { CL_NONE = 0, CL_PROBE, CL_RAW, CL_HTTP };

static int   g_listen = -1;
static int   g_client = -1;
static int   g_cstate = CL_NONE;
static int   g_probe_ticks;
static int   g_stdout;
static int   g_port;
static char  g_netdesc[96];
static char  g_url[96];

/* ======================================================================== */
/*  WAV                                                                     */
/* ======================================================================== */

/* Everything in a RIFF/WAVE header is little-endian, so we assemble it byte
 * by byte instead of memcpy-ing structs. That keeps the file format correct
 * on a big-endian machine and, more usefully, makes the layout legible: you
 * can read the 44 bytes off the page and match them against a hex dump. */
static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v      );
    p[1] = (unsigned char)(v >>  8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v     );
    p[1] = (unsigned char)(v >> 8);
}

static void wav_header(unsigned char *h, int rate, uint32_t data_bytes)
{
    const uint16_t chans = 1;
    const uint16_t bits  = 16;
    const uint16_t align = (uint16_t)(chans * bits / 8);

    memcpy(h + 0,  "RIFF", 4);
    put_u32(h + 4,  36u + data_bytes);   /* everything after this field   */
    memcpy(h + 8,  "WAVE", 4);

    memcpy(h + 12, "fmt ", 4);
    put_u32(h + 16, 16);                 /* fmt chunk size, PCM           */
    put_u16(h + 20, 1);                  /* format 1 = uncompressed PCM   */
    put_u16(h + 22, chans);
    put_u32(h + 24, (uint32_t)rate);
    put_u32(h + 28, (uint32_t)rate * align);   /* bytes per second        */
    put_u16(h + 32, align);              /* bytes per frame               */
    put_u16(h + 34, bits);

    memcpy(h + 36, "data", 4);
    put_u32(h + 40, data_bytes);
}

int sink_rec_start(int rate, char *path_out, size_t pathsz,
                   char *err, size_t errsz)
{
    if (g_wav) return 0;

    time_t     now = time(NULL);
    struct tm  tmv;
    localtime_r(&now, &tmv);
    strftime(g_wav_path, sizeof g_wav_path, "bb-%Y%m%d-%H%M%S.wav", &tmv);

    g_wav = fopen(g_wav_path, "wb");
    if (!g_wav) {
        snprintf(err, errsz, "cannot create %s: %s", g_wav_path, strerror(errno));
        return -1;
    }

    /* The sample rate is frozen at record-start. If you sweep the rate knob
     * mid-recording the file keeps this header, so the pitch shift you hear
     * live is baked into the file exactly as it happened -- which is the
     * honest result, and the one you want when the file is going to hardware. */
    g_wav_rate   = rate;
    g_wav_frames = 0;

    unsigned char h[44];
    wav_header(h, rate, 0);
    if (fwrite(h, 1, 44, g_wav) != 44) {
        snprintf(err, errsz, "cannot write header: %s", strerror(errno));
        fclose(g_wav);
        g_wav = NULL;
        return -1;
    }

    /* Start reading the ring from wherever the audio thread is NOW, not from
     * whatever stale content is sitting in it. */
    atomic_store(&bb.file_r, atomic_load(&bb.sink_w));

    if (path_out) snprintf(path_out, pathsz, "%s", g_wav_path);
    return 0;
}

void sink_rec_stop(void)
{
    if (!g_wav) return;

    /* Go back and fill in the two sizes we could not know when we started.
     * This is why a .wav is not a streaming format and why a recording
     * killed with SIGKILL has zeroes here and confuses most players. */
    uint32_t data_bytes = g_wav_frames * 2u;
    unsigned char h[44];
    wav_header(h, g_wav_rate, data_bytes);

    fflush(g_wav);
    if (fseek(g_wav, 0, SEEK_SET) == 0)
        fwrite(h, 1, 44, g_wav);

    fclose(g_wav);
    g_wav = NULL;
}

int         sink_rec_active(void) { return g_wav != NULL; }
unsigned    sink_rec_frames(void) { return g_wav_frames; }
const char *sink_rec_path(void)   { return g_wav_path; }

/* ======================================================================== */
/*  Network                                                                 */
/* ======================================================================== */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Work out the address a listener should actually aim at, so the UI can
 * print a URL you can paste rather than a port number you have to combine
 * with an IP you have to go and look up.
 *
 * Best source by far is SSH_CONNECTION, whose third field is the address on
 * THIS box that your ssh client connected to. If you reached the machine
 * over Tailscale, that is the Tailscale address; over LAN, the LAN address.
 * Either way it is provably reachable from wherever you are sitting,
 * because you are already using it.
 *
 * Falling back: first non-loopback IPv4 interface, then localhost. */
static void discover_url(int local_only, int port)
{
    char host[64] = "";

    if (local_only) {
        snprintf(g_url, sizeof g_url, "http://127.0.0.1:%d", port);
        return;
    }

    const char *sc = getenv("SSH_CONNECTION");
    if (sc) {
        /* "<client ip> <client port> <server ip> <server port>" */
        const char *p = sc;
        int field = 0;
        while (*p && field < 2) { if (*p == ' ') field++; p++; }
        if (field == 2) {
            size_t i = 0;
            while (p[i] && p[i] != ' ' && i < sizeof host - 1) { host[i] = p[i]; i++; }
            host[i] = '\0';
            if (strchr(host, ':')) host[0] = '\0';    /* IPv6, skip bracketing */
        }
    }

    if (!host[0]) {
        struct ifaddrs *ifa = NULL;
        if (getifaddrs(&ifa) == 0) {
            for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                struct sockaddr_in *sa = (struct sockaddr_in *)p->ifa_addr;
                if (sa->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;
                snprintf(host, sizeof host, "%s", inet_ntoa(sa->sin_addr));
                break;
            }
            freeifaddrs(ifa);
        }
    }

    if (!host[0]) snprintf(host, sizeof host, "localhost");
    snprintf(g_url, sizeof g_url, "http://%s:%d", host, port);
}

const char *sink_stream_url(void) { return g_url; }

int sink_init(int port, int local_only, int use_stdout, char *err, size_t errsz)
{
    g_stdout = use_stdout;
    g_port   = port;
    g_netdesc[0] = '\0';

    if (use_stdout) {
        set_nonblock(STDOUT_FILENO);
        snprintf(g_netdesc, sizeof g_netdesc, "stdout");
    }

    if (port <= 0) return 0;

    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) {
        snprintf(err, errsz, "socket: %s", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons((uint16_t)port);
    a.sin_addr.s_addr = local_only ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);

    if (bind(g_listen, (struct sockaddr *)&a, sizeof a) < 0) {
        snprintf(err, errsz, "bind :%d: %s", port, strerror(errno));
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    if (listen(g_listen, 1) < 0) {
        snprintf(err, errsz, "listen: %s", strerror(errno));
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    set_nonblock(g_listen);

    discover_url(local_only, port);
    snprintf(g_netdesc, sizeof g_netdesc, "waiting");
    return 0;
}

void sink_close(void)
{
    sink_rec_stop();
    if (g_client >= 0) { close(g_client); g_client = -1; g_cstate = CL_NONE; }
    if (g_listen >= 0) { close(g_listen); g_listen = -1; }
}

int sink_net_state(void)
{
    if (g_cstate == CL_RAW || g_cstate == CL_HTTP) return 2;   /* streaming */
    if (g_stdout && g_listen < 0)                  return 2;
    if (g_listen >= 0)                             return 1;   /* waiting   */
    return 0;
}

const char *sink_net_desc(void) { return g_netdesc; }

static void accept_pending(void)
{
    if (g_listen < 0) return;
    for (;;) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int fd = accept(g_listen, (struct sockaddr *)&ca, &cl);
        if (fd < 0) return;

        /* Newest connection wins. Reconnecting from the laptop should just
         * work rather than being refused because a dead session still holds
         * the slot. */
        if (g_client >= 0) close(g_client);
        g_client      = fd;
        g_cstate      = CL_PROBE;
        g_probe_ticks = 0;
        set_nonblock(g_client);

        /* Audio is a steady trickle of small writes; Nagle would coalesce
         * them into latency for no benefit. */
        int one = 1;
        setsockopt(g_client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        snprintf(g_netdesc, sizeof g_netdesc, "%s:%d <- %s",
                 "0.0.0.0", g_port, inet_ntoa(ca.sin_addr));
    }
}

/* Decide what kind of client we just accepted.
 *
 * Called once per UI frame (~20ms). A browser's request lands on the first
 * or second call; a raw listener never sends anything, so after ~300ms of
 * silence we assume raw. Returns 1 once the client is ready to be streamed
 * to, 0 while still undecided. */
static int probe_client(void)
{
    if (g_cstate != CL_PROBE) return g_cstate == CL_RAW || g_cstate == CL_HTTP;

    char req[512];
    ssize_t n = recv(g_client, req, sizeof req - 1, MSG_DONTWAIT);

    if (n > 0) {
        req[n] = '\0';
        if (!strncmp(req, "GET", 3) || !strncmp(req, "HEAD", 4)) {
            int rate = atomic_load(&bb.rate);

            /* Pull the path out of "GET /what/ever HTTP/1.1". */
            char path[64] = "/";
            const char *sp = strchr(req, ' ');
            if (sp) {
                sp++;
                size_t i = 0;
                while (sp[i] && sp[i] != ' ' && sp[i] != '?' && i < sizeof path - 1) {
                    path[i] = sp[i];
                    i++;
                }
                path[i] = '\0';
            }

            if (!strcmp(path, "/favicon.ico")) {
                const char *nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                                 "Connection: close\r\n\r\n";
                send(g_client, nf, strlen(nf), MSG_NOSIGNAL);
                close(g_client); g_client = -1; g_cstate = CL_NONE;
                return 0;
            }

            /* Navigating to "/" must return a PAGE, not the audio itself.
             * Browsers download a bare audio/wav with no Content-Length
             * instead of playing it -- an endless file save, which is not
             * what anyone wants. An <audio> element pointed at the stream
             * plays it in place and gives you a transport. */
            if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
                char page[2048];
                int pl = snprintf(page, sizeof page,
"<!doctype html><html><head><meta charset=\"utf-8\"><title>bytebeat</title>"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<style>body{background:#0d0d0f;color:#c8c8d0;font:15px/1.6 ui-monospace,Menlo,"
"Consolas,monospace;margin:0;padding:6vh 6vw;}h1{color:#e05a5a;font-size:22px;"
"letter-spacing:.22em;margin:0 0 4px;}p{margin:.5em 0}code{background:#1a1a1f;"
"color:#8fd18f;padding:2px 6px;border-radius:3px;display:inline-block;"
"word-break:break-all}audio{width:100%%;max-width:560px;margin:18px 0}"
"a{color:#7aa7d8}.d{color:#666;font-size:13px}</style></head><body>"
"<h1>BYTEBEAT</h1>"
"<p class=\"d\">live from %s &middot; %d Hz &middot; mono 16-bit</p>"
"<audio controls autoplay src=\"/stream.wav\"></audio>"
"<p class=\"d\">If it does not start, press play. If it stalls or you change "
"the sample rate, reload.</p>"
"<p class=\"d\">Lower latency, from a terminal <b>on this machine</b>:</p>"
"<p><code>ffplay -nodisp -autoexit %s/stream.wav</code></p>"
"<p><code>mpv --no-video %s/stream.wav</code></p>"
"<p><code>vlc %s/stream.wav</code></p>"
"<p class=\"d\">Raw headerless PCM is on the same port for "
"<code>nc</code>-style clients.</p>"
"</body></html>",
                    g_url, rate, g_url, g_url, g_url);

                char hh[256];
                int hl = snprintf(hh, sizeof hh,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                    "Content-Length: %d\r\nConnection: close\r\n\r\n", pl);
                send(g_client, hh, (size_t)hl, MSG_NOSIGNAL);
                if (strncmp(req, "HEAD", 4))
                    send(g_client, page, (size_t)pl, MSG_NOSIGNAL);
                close(g_client); g_client = -1; g_cstate = CL_NONE;
                snprintf(g_netdesc, sizeof g_netdesc, "served page");
                return 0;
            }

            /* Streaming WAV: the length fields are unknowable because the
             * stream never ends, so we use the conventional 0xffffffff.
             * Players treat that as "keep going until the socket closes". */
            unsigned char h[44];
            wav_header(h, rate, 0xffffffffu);
            h[4] = h[5] = h[6] = h[7] = 0xff;      /* RIFF size = unknown */

            char hdr[320];
            int hl = snprintf(hdr, sizeof hdr,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: audio/wav\r\n"
                "Cache-Control: no-cache, no-store\r\n"
                "Pragma: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n");

            int head_only = !strncmp(req, "HEAD", 4);
            send(g_client, hdr, (size_t)hl, MSG_NOSIGNAL);
            if (head_only) { close(g_client); g_client = -1; g_cstate = CL_NONE; return 0; }
            send(g_client, h, 44, MSG_NOSIGNAL);

            g_cstate = CL_HTTP;
            snprintf(g_netdesc, sizeof g_netdesc, ":%d http/wav %dHz", g_port, rate);
        } else {
            g_cstate = CL_RAW;
        }
    } else if (n == 0) {
        close(g_client); g_client = -1; g_cstate = CL_NONE;   /* client left */
        return 0;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        close(g_client); g_client = -1; g_cstate = CL_NONE;
        return 0;
    } else if (++g_probe_ticks > 15) {
        g_cstate = CL_RAW;                       /* silent => nc / ffplay */
        snprintf(g_netdesc, sizeof g_netdesc, ":%d raw pcm", g_port);
    }

    if (g_cstate == CL_RAW || g_cstate == CL_HTTP) {
        /* Start them at the live edge, not at whatever stale audio was
         * sitting in the ring while we were probing. */
        atomic_store(&bb.net_r, atomic_load(&bb.sink_w));
        return 1;
    }
    return 0;
}

/* ======================================================================== */
/*  Draining the ring                                                       */
/* ======================================================================== */

/* Returns how many samples are readable from cursor `r`, dropping the oldest
 * if the producer has lapped us. Unsigned arithmetic wraps correctly here:
 * (w - r) is the true distance even across the 2^32 rollover of the cursors,
 * which is why both are plain unsigned counters rather than masked indices. */
static unsigned readable(unsigned w, unsigned *r)
{
    unsigned avail = w - *r;
    if (avail > BB_SINK_LEN) {
        atomic_fetch_add(&bb.sink_lost, (int)(avail - BB_SINK_LEN));
        *r    = w - BB_SINK_LEN;
        avail = BB_SINK_LEN;
    }
    return avail;
}

void sink_service(void)
{
    accept_pending();

    unsigned w = atomic_load_explicit(&bb.sink_w, memory_order_acquire);

    /* ---- file ---------------------------------------------------------- */
    if (g_wav) {
        unsigned r = atomic_load_explicit(&bb.file_r, memory_order_relaxed);
        unsigned avail = readable(w, &r);
        while (avail > 0) {
            unsigned idx = r & BB_SINK_MASK;
            unsigned run = BB_SINK_LEN - idx;      /* to end of the array   */
            if (run > avail) run = avail;
            size_t got = fwrite(&bb.sink[idx], sizeof(int16_t), run, g_wav);
            g_wav_frames += (unsigned)got;
            r     += (unsigned)got;
            avail -= (unsigned)got;
            if (got != run) break;                 /* disk full, give up    */
        }
        atomic_store_explicit(&bb.file_r, r, memory_order_relaxed);
    } else {
        /* Not recording: keep the cursor pinned to the head so `readable()`
         * never reports a lap we do not care about. */
        atomic_store_explicit(&bb.file_r, w, memory_order_relaxed);
    }

    /* ---- network / stdout ---------------------------------------------- */
    int client_ready = (g_client >= 0) ? probe_client() : 0;
    int fd = client_ready ? g_client : (g_stdout ? STDOUT_FILENO : -1);
    if (fd >= 0) {
        unsigned r = atomic_load_explicit(&bb.net_r, memory_order_relaxed);
        unsigned avail = readable(w, &r);
        while (avail > 0) {
            unsigned idx = r & BB_SINK_MASK;
            unsigned run = BB_SINK_LEN - idx;
            if (run > avail) run = avail;

            ssize_t nw = send(fd, &bb.sink[idx], (size_t)run * sizeof(int16_t),
                              MSG_NOSIGNAL);
            if (nw < 0 && errno == ENOTSOCK)            /* stdout, not a socket */
                nw = write(fd, &bb.sink[idx], (size_t)run * sizeof(int16_t));

            if (nw <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    break;   /* client is behind; the ring will drop for us */
                if (g_client >= 0 && fd == g_client) {
                    close(g_client);
                    g_client = -1;
                    g_cstate = CL_NONE;
                    snprintf(g_netdesc, sizeof g_netdesc, "waiting");
                } else {
                    g_stdout = 0;
                }
                break;
            }
            unsigned sent = (unsigned)(nw / (ssize_t)sizeof(int16_t));
            r     += sent;
            avail -= sent;
            if (sent < run) break;                 /* partial write         */
        }
        atomic_store_explicit(&bb.net_r, r, memory_order_relaxed);
    } else {
        atomic_store_explicit(&bb.net_r, w, memory_order_relaxed);
    }
}
