/*
 * sipfax-slmodem-bridge — SIPfax <-> SmartLink slmodemd audio/control bridge.
 *
 * Lets SIPfax drive the slmodem datapump (V.22bis / V.32bis / V.34) in place of
 * the spandsp worker, honoring the exact same process contract as
 * vendor/sipfax-softmodem/sipfax-softmodem.c so the Node side is unchanged:
 *
 *   stdin   : [uint16 BE len][G.711 payload]  (160 samples / 20ms @ 8kHz)
 *   stdout  : same framing (modem-generated audio back to RTP)
 *   fd 3    : newline-delimited JSON control events; emits {"event":"pty-opened",
 *             "slavePath":"/dev/ttySLn"} so pppd-supervisor attaches pppd there.
 *
 * Topology (see DESIGN.md): this binary runs in two modes from ONE executable.
 *   - bridge mode (spawned by Node): owns stdin/stdout/fd3, spawns slmodemd with
 *     `-e <self>`, drives the AT answer sequence on /dev/ttySLn, and pumps audio
 *     between Node (G.711 8kHz) and slmodemd (S16LE 9600Hz) with resampling.
 *   - shim mode (forked by slmodemd at off-hook with argv = dial_string, audiofd):
 *     splices slmodemd's audio socket <-> the bridge's AF_UNIX audio socket.
 *
 * Only slmodemd must be 32-bit (it links dsplibs.o); this bridge is native.
 *
 * Copyright 2026. Released under GPL-2.0 (derived from D-Modem, GPL-2.0).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#define RTP_RATE        8000
#define MODEM_RATE_DEFAULT 9600
#define FRAME_MS        20
#define RTP_SAMPLES     (RTP_RATE * FRAME_MS / 1000)            /* 160 */
#define MODEM_SAMPLES   (MODEM_RATE_DEFAULT * FRAME_MS / 1000)  /* 192, buffer sizing */
/* slmodem's internal sample rate. Default 9600 (then the bridge resamples to/from
 * the 8 kHz RTP rate). If slmodem is built for 8000 (SIPFAX_MODEM_RATE=8000), the
 * bridge passes audio through unresampled — no resampling distortion. */
static int g_modem_rate = MODEM_RATE_DEFAULT;
#define MAX_PAYLOAD     4096
#define TTY_PATH_DEFAULT "/dev/ttySL0"
static const char *g_tty = TTY_PATH_DEFAULT;   /* override via SIPFAX_MODEM_TTY */

static volatile sig_atomic_t g_stop = 0;
static pid_t g_slmodemd_pid = -1;
static char  g_audio_sock_path[128] = {0};

static void logmsg(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "[slmodem-bridge] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------------ G.711 */
/* Classic CCITT reference conversions (public domain). */
#define G711_BIAS 0x84
#define G711_CLIP 8159

static int16_t ulaw2linear(uint8_t u) {
    u = ~u;
    int t = ((u & 0x0f) << 3) + G711_BIAS;
    t <<= ((unsigned)u & 0x70) >> 4;
    return (int16_t)((u & 0x80) ? (G711_BIAS - t) : (t - G711_BIAS));
}

static uint8_t linear2ulaw(int16_t pcm) {
    int sign = (pcm >> 8) & 0x80;
    if (sign) pcm = (int16_t)-pcm;
    if (pcm > G711_CLIP) pcm = G711_CLIP;
    int mag = pcm + G711_BIAS;
    int exp = 7;
    for (int mask = 0x4000; exp > 0 && (mag & mask) == 0; mask >>= 1) exp--;
    int man = (mag >> (exp + 3)) & 0x0f;
    return (uint8_t)(~(sign | (exp << 4) | man));
}

static int16_t alaw2linear(uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0f) << 4;
    int seg = ((unsigned)a & 0x70) >> 4;
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else { t += 0x108; t <<= seg - 1; }
    return (int16_t)((a & 0x80) ? t : -t);
}

static uint8_t linear2alaw(int16_t pcm) {
    int sign = ((~pcm) >> 8) & 0x80;
    if (!sign) pcm = (int16_t)-pcm;
    if (pcm > 0x7fff) pcm = 0x7fff;
    uint8_t a;
    if (pcm < 256) {
        a = (uint8_t)(pcm >> 4);
    } else {
        int exp = 7;
        for (int mask = 0x4000; exp > 1 && (pcm & mask) == 0; mask >>= 1) exp--;
        int man = (pcm >> (exp + 3)) & 0x0f;
        a = (uint8_t)((exp << 4) | man);
    }
    return (uint8_t)((a ^ 0x55) | sign);
}

/* codec: 0 = mu-law (PCMU), 1 = A-law (PCMA) */
static int g_codec = 0;

static void g711_decode(const uint8_t *in, int n, int16_t *out) {
    if (g_codec == 1) for (int i = 0; i < n; i++) out[i] = alaw2linear(in[i]);
    else              for (int i = 0; i < n; i++) out[i] = ulaw2linear(in[i]);
}
static void g711_encode(const int16_t *in, int n, uint8_t *out) {
    if (g_codec == 1) for (int i = 0; i < n; i++) out[i] = linear2alaw(in[i]);
    else              for (int i = 0; i < n; i++) out[i] = linear2ulaw(in[i]);
}

/* ------------------------------------------ streaming polyphase FIR resampler
 * slmodem's datapump is tuned for a 9600 Hz internal rate, so the bridge must
 * resample between the 8 kHz RTP audio and slmodem's 9600 Hz. (Running slmodem
 * at 8000 mis-tunes every modem tone by 8000/9600 and real modems won't answer.)
 *
 * The filter is critical: linear interpolation droops ~6 dB across the upper
 * voiceband (measured), which strangles the high-frequency carriers V.32bis/V.34
 * need and caps the link rate. This windowed-sinc polyphase FIR (fc=3950 Hz,
 * 64 taps/branch) is flat to +/-0.0 dB through 3700 Hz (offline-measured), i.e.
 * transparent across the entire modem band. `passthrough` covers in==out. */
typedef struct {
    int passthrough;             /* 1 when in_rate == out_rate: copy, no filtering */
    int L, M, taps, plen, phase;
    double *proto;               /* prototype filter, length L*taps */
    double *hist;                /* input history, hist[0] = newest */
} resamp_t;

static int igcd(int a, int b) { while (b) { int t = a % b; a = b; b = t; } return a; }

static void resamp_init(resamp_t *r, int in_rate, int out_rate) {
    if (in_rate == out_rate) { r->passthrough = 1; return; }   /* no resampling needed */
    r->passthrough = 0;
    int g = igcd(in_rate, out_rate);
    r->L = out_rate / g;           /* interpolation factor (8000->9600 => 6) */
    r->M = in_rate / g;            /* decimation factor    (8000->9600 => 5) */
    r->taps = 64;                  /* taps per polyphase branch */
    r->plen = r->L * r->taps;
    r->phase = 0;
    r->proto = malloc(sizeof(double) * (size_t)r->plen);
    r->hist = calloc((size_t)r->taps, sizeof(double));
    const double Fs_up = (double)in_rate * r->L;   /* upsampled rate */
    const double fc = 3950.0;                      /* just under 4 kHz Nyquist */
    const double c = (r->plen - 1) / 2.0;
    double sum = 0.0;
    for (int i = 0; i < r->plen; i++) {
        double x = 2.0 * fc / Fs_up * (i - c);
        double s = (fabs(x) < 1e-9) ? 1.0 : sin(M_PI * x) / (M_PI * x);
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * i / (r->plen - 1));   /* Hamming */
        r->proto[i] = (2.0 * fc / Fs_up) * s * w;
        sum += r->proto[i];
    }
    double scale = (double)r->L / sum;             /* unity passband gain */
    for (int i = 0; i < r->plen; i++) r->proto[i] *= scale;
}

static int resamp_run(resamp_t *r, const int16_t *in, int n, int16_t *out, int outcap) {
    if (r->passthrough) {
        int c = n < outcap ? n : outcap;
        for (int i = 0; i < c; i++) out[i] = in[i];
        return c;
    }
    int o = 0;
    for (int i = 0; i < n; i++) {
        for (int k = r->taps - 1; k > 0; k--) r->hist[k] = r->hist[k - 1];
        r->hist[0] = (double)in[i];
        while (r->phase < r->L && o < outcap) {
            double acc = 0.0;
            int br = r->phase;
            for (int k = 0; k < r->taps; k++) acc += r->proto[br + k * r->L] * r->hist[k];
            long iv = lround(acc);
            if (iv > 32767) iv = 32767; else if (iv < -32768) iv = -32768;
            out[o++] = (int16_t)iv;
            r->phase += r->M;
        }
        r->phase -= r->L;
    }
    return o;
}

/* ----------------------------------------------------------------- io utils */
static int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = buf; size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0;            /* EOF */
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        got += (size_t)r;
    }
    return 1;
}
static int write_full(int fd, const void *buf, size_t n) {
    const uint8_t *p = buf; size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, p + put, n - put);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        put += (size_t)w;
    }
    return 1;
}

/* Read one SIPfax frame: [u16 BE len][payload]. Returns payload len, 0 EOF, -1 err. */
static int read_node_frame(int fd, uint8_t *payload, int cap) {
    uint8_t hdr[2];
    int r = read_full(fd, hdr, 2);
    if (r <= 0) return r;
    int len = (hdr[0] << 8) | hdr[1];
    if (len > cap) return -1;
    r = read_full(fd, payload, (size_t)len);
    if (r <= 0) return r ? r : -1;
    return len;
}
static int write_node_frame(int fd, const uint8_t *payload, int len) {
    uint8_t hdr[2] = { (uint8_t)(len >> 8), (uint8_t)(len & 0xff) };
    if (write_full(fd, hdr, 2) < 0) return -1;
    return write_full(fd, payload, (size_t)len);
}

static void emit_control(const char *json) { dprintf(3, "%s\n", json); }

/* --------------------------------------------------------------- shim mode */
static void shimlog(const char *fmt, ...) {
    FILE *f = fopen("/tmp/sipfax-shim.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static int run_shim(int audiofd) {
    const char *path = getenv("SIPFAX_AUDIO_SOCK");
    if (!path) { shimlog("shim: SIPFAX_AUDIO_SOCK unset"); return 1; }

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { shimlog("shim: socket: %s", strerror(errno)); return 1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    int connected = -1;
    for (int attempt = 0; attempt < 50; attempt++) {       /* ~5s */
        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) { connected = 0; break; }
        usleep(100000);
    }
    if (connected < 0) { shimlog("shim: connect %s: %s", path, strerror(errno)); return 1; }
    shimlog("shim: connected audio splice (audiofd=%d uid=%d)", audiofd, (int)getuid());

    /* Bidirectional byte splice between slmodemd's audio socket and the bridge. */
    uint8_t buf[8192];
    for (;;) {
        fd_set rs; FD_ZERO(&rs);
        FD_SET(audiofd, &rs); FD_SET(s, &rs);
        int mx = audiofd > s ? audiofd : s;
        int rv = select(mx + 1, &rs, NULL, NULL, NULL);
        if (rv < 0) { if (errno == EINTR) continue; break; }
        if (FD_ISSET(audiofd, &rs)) {
            ssize_t n = read(audiofd, buf, sizeof(buf));
            if (n <= 0) break;
            if (write_full(s, buf, (size_t)n) < 0) break;
        }
        if (FD_ISSET(s, &rs)) {
            ssize_t n = read(s, buf, sizeof(buf));
            if (n <= 0) break;
            if (write_full(audiofd, buf, (size_t)n) < 0) break;
        }
    }
    close(s);
    return 0;
}

/* ------------------------------------------------------------- bridge mode */
static const char *modulation_at_command(void) {
    const char *m = getenv("SIPFAX_MODEM_MODULATION");
    if (!m || !*m) m = "v22bis";
    if (!strcasecmp(m, "v34"))    return "AT+MS=34,1,2400,33600";
    if (!strcasecmp(m, "v32bis")) return "AT+MS=132,1,4800,14400";
    if (!strcasecmp(m, "v32"))    return "AT+MS=32,1,4800,9600";
    return "AT+MS=122,1,1200,2400"; /* v22bis (default, Phase 1 parity) */
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void cleanup(void) {
    if (g_slmodemd_pid > 0) { kill(g_slmodemd_pid, SIGTERM); }
    if (g_audio_sock_path[0]) unlink(g_audio_sock_path);
}

static int open_tty_raw(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
        tcsetattr(fd, TCSANOW, &t);
    }
    return fd;
}

/* Send an AT command and wait briefly for an OK/ERROR/CONNECT token. */
static void at_send(int fd, const char *cmd) {
    char line[128];
    int n = snprintf(line, sizeof(line), "%s\r", cmd);
    write_full(fd, line, (size_t)n);
    logmsg("AT> %s", cmd);
    usleep(200000);
}

static int spawn_slmodemd(const char *self_path) {
    const char *slmodemd = getenv("SIPFAX_SLMODEMD");
    if (!slmodemd || !*slmodemd) slmodemd = "slmodemd";
    /* Optional positional device name (e.g. /dev/slamr1 -> /dev/ttySL1) so a
     * second instance can coexist; used by the loopback test. */
    const char *dev = getenv("SIPFAX_SLMODEM_DEV");

    pid_t pid = fork();
    if (pid < 0) { logmsg("fork slmodemd: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        /* child: slmodemd. Keep stdin off Node's pipe; logs to our stderr.
         * -n = regular (non-realtime) priority: safer on a VM and our pacing is
         * driven by the RTP frame cadence anyway. */
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
        dup2(2, 1);                         /* slmodemd stdout -> our stderr */
        if (dev && *dev)
            execlp(slmodemd, slmodemd, "-n", "-e", self_path, dev, (char *)NULL);
        else
            execlp(slmodemd, slmodemd, "-n", "-e", self_path, (char *)NULL);
        fprintf(stderr, "[slmodem-bridge] execlp slmodemd: %s\n", strerror(errno));
        _exit(127);
    }
    g_slmodemd_pid = pid;
    return 0;
}

static int run_bridge(const char *self_path) {
    /* codec selection from Node env */
    const char *codec = getenv("SIPFAX_MODEM_CODEC");
    if (codec && (!strcasecmp(codec, "PCMA") || !strcasecmp(codec, "alaw"))) g_codec = 1;
    const char *ttyenv = getenv("SIPFAX_MODEM_TTY");
    if (ttyenv && *ttyenv) g_tty = ttyenv;
    const char *mr = getenv("SIPFAX_MODEM_RATE");
    if (mr && atoi(mr) > 0) g_modem_rate = atoi(mr);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    atexit(cleanup);

    /* AF_UNIX listening socket for the audio splice (shim connects here). */
    snprintf(g_audio_sock_path, sizeof(g_audio_sock_path),
             "/tmp/sipfax-slmodem-%d.sock", (int)getpid());
    unlink(g_audio_sock_path);
    int lsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lsock < 0) { logmsg("listen socket: %s", strerror(errno)); return 1; }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_audio_sock_path, sizeof(addr.sun_path) - 1);
    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        logmsg("bind %s: %s", g_audio_sock_path, strerror(errno)); return 1;
    }
    chmod(g_audio_sock_path, 0777);   /* shim runs as 'nobody' after priv-drop */
    listen(lsock, 1);
    setenv("SIPFAX_AUDIO_SOCK", g_audio_sock_path, 1);

    if (spawn_slmodemd(self_path) < 0) return 1;
    emit_control("{\"event\":\"started\",\"engine\":\"slmodem\"}");

    /* Wait for slmodemd to create the modem tty. */
    int tries = 0;
    while (access(g_tty, F_OK) != 0 && tries++ < 100 && !g_stop) usleep(100000);
    if (access(g_tty, F_OK) != 0) { logmsg("%s never appeared", g_tty); return 1; }
    int tty = open_tty_raw(g_tty);
    if (tty < 0) { logmsg("open %s: %s", g_tty, strerror(errno)); return 1; }
    logmsg("modem tty ready: %s", g_tty);

    /* AT setup: echo off, ignore dial tone, select modulation, then go off-hook.
     * Default is answer (SIPfax answers inbound calls); SIPFAX_MODEM_DIAL forces
     * originate mode (used only by the loopback test harness). */
    at_send(tty, "ATE0");
    at_send(tty, "ATX3");
    at_send(tty, modulation_at_command());
    const char *dial = getenv("SIPFAX_MODEM_DIAL");
    if (dial && *dial) {
        char d[64]; snprintf(d, sizeof(d), "ATD%s", dial);
        at_send(tty, d);   /* originate */
    } else {
        at_send(tty, "ATA");   /* answer */
    }

    /* Accept the audio splice connection from the shim. */
    int afd = -1;
    {
        fd_set rs; FD_ZERO(&rs); FD_SET(lsock, &rs);
        struct timeval tv = { 5, 0 };
        if (select(lsock + 1, &rs, NULL, NULL, &tv) > 0)
            afd = accept(lsock, NULL, NULL);
    }
    if (afd < 0) { logmsg("no audio splice connection (ATA failed?)"); return 1; }
    logmsg("audio splice connected");

    resamp_t up, down;            /* up: RTP->modem, down: modem->RTP (identity if 8000) */
    resamp_init(&up, RTP_RATE, g_modem_rate);
    resamp_init(&down, g_modem_rate, RTP_RATE);
    logmsg("modem rate %d Hz (%s)", g_modem_rate,
           g_modem_rate == RTP_RATE ? "passthrough" : "resampling");

    /* Prime slmodemd with one 20 ms frame of silence (mirrors D-Modem). */
    { int16_t sil[MODEM_SAMPLES] = {0}; int ns = g_modem_rate * FRAME_MS / 1000;
      if (ns > MODEM_SAMPLES) ns = MODEM_SAMPLES; write_full(afd, sil, (size_t)ns * 2); }

    bool connected = false;
    char ttybuf[256]; int ttylen = 0;
    uint8_t pay[MAX_PAYLOAD];
    int16_t pcm8[MAX_PAYLOAD], pcm96[MAX_PAYLOAD * 2];

    while (!g_stop) {
        fd_set rs; FD_ZERO(&rs);
        FD_SET(0, &rs);            /* Node stdin (G.711) */
        FD_SET(afd, &rs);          /* modem audio (S16 9600) */
        int mx = afd;
        if (!connected) { FD_SET(tty, &rs); if (tty > mx) mx = tty; }
        struct timeval tv = { 1, 0 };
        int rv = select(mx + 1, &rs, NULL, NULL, &tv);
        if (rv < 0) { if (errno == EINTR) continue; break; }

        /* Node -> modem: G.711 8k -> S16 -> resample to 9600 -> audio socket. */
        if (FD_ISSET(0, &rs)) {
            int len = read_node_frame(0, pay, sizeof(pay));
            if (len <= 0) { logmsg("node stdin closed"); break; }
            g711_decode(pay, len, pcm8);
            int n = resamp_run(&up, pcm8, len, pcm96, sizeof(pcm96) / 2);
            if (write_full(afd, pcm96, (size_t)n * 2) < 0) break;
        }

        /* modem -> Node: S16 9600 -> resample to 8000 -> G.711 -> stdout frame. */
        if (FD_ISSET(afd, &rs)) {
            uint8_t raw[MODEM_SAMPLES * 2 * 4];
            ssize_t n = read(afd, raw, sizeof(raw));
            if (n <= 0) { logmsg("audio splice closed"); break; }
            int nsamp = (int)n / 2;
            int m = resamp_run(&down, (int16_t *)raw, nsamp, pcm8, sizeof(pcm8) / 2);
            uint8_t out[MAX_PAYLOAD];
            for (int off = 0; off < m; off += RTP_SAMPLES) {
                int chunk = (m - off) < RTP_SAMPLES ? (m - off) : RTP_SAMPLES;
                g711_encode(pcm8 + off, chunk, out);
                if (write_node_frame(1, out, chunk) < 0) { g_stop = 1; break; }
            }
        }

        /* tty -> watch for CONNECT, then hand the tty to pppd. */
        if (!connected && FD_ISSET(tty, &rs)) {
            ssize_t n = read(tty, ttybuf + ttylen, sizeof(ttybuf) - 1 - ttylen);
            if (n > 0) {
                ttylen += (int)n; ttybuf[ttylen] = 0;
                if (strstr(ttybuf, "CONNECT")) {
                    /* strip CR/LF so the negotiated rate (e.g. "CONNECT 33600") logs cleanly */
                    char rate[64]; int ri = 0;
                    for (const char *q = strstr(ttybuf, "CONNECT");
                         *q && *q != '\r' && *q != '\n' && ri < (int)sizeof(rate) - 1; q++)
                        rate[ri++] = *q;
                    rate[ri] = 0;
                    logmsg("%s; handing %s to pppd", rate, g_tty);
                    char ctl[256];
                    snprintf(ctl, sizeof(ctl),
                             "{\"event\":\"pty-opened\",\"slavePath\":\"%s\",\"engine\":\"slmodem\"}",
                             g_tty);
                    emit_control(ctl);
                    close(tty);          /* release the tty so pppd owns it */
                    connected = true;
                } else if (strstr(ttybuf, "NO CARRIER") || strstr(ttybuf, "ERROR") ||
                           strstr(ttybuf, "BUSY") || strstr(ttybuf, "NO ANSWER")) {
                    logmsg("modem reported failure: %.*s", ttylen, ttybuf);
                    break;
                }
                if (ttylen > 200) ttylen = 0;   /* keep buffer bounded */
            }
        }
    }

    if (afd >= 0) close(afd);
    emit_control("{\"event\":\"pty-closed\",\"engine\":\"slmodem\"}");
    return 0;
}

/* --------------------------------------------------------------------- main */
int main(int argc, char *argv[]) {
    /* Shim mode: any process started with SIPFAX_AUDIO_SOCK already in its
     * environment was exec'd by slmodemd as the `-e` audio program (the parent
     * bridge sets that var only AFTER deciding it is the bridge). Detecting on
     * the env — not argc — guarantees a child can NEVER re-enter bridge mode and
     * spawn another (RT-priority) slmodemd. slmodemd exec's us as
     * `<self> <dial_string> <audiofd>`. */
    if (getenv("SIPFAX_AUDIO_SOCK")) {
        int audiofd = -1;
        if (argc >= 3) {
            const char *p = argv[argc - 1];   /* last arg is the fd */
            bool numeric = *p != 0;
            for (const char *q = p; *q; q++) if (!isdigit((unsigned char)*q)) numeric = false;
            if (numeric) audiofd = atoi(p);
        }
        shimlog("shim: argc=%d argv1='%s' last='%s' fd=%d sock=%s",
                argc, argc > 1 ? argv[1] : "", argc > 0 ? argv[argc - 1] : "",
                audiofd, getenv("SIPFAX_AUDIO_SOCK"));
        if (audiofd < 0) { shimlog("shim: no audiofd in argv; refusing to spawn"); return 1; }
        return run_shim(audiofd);
    }

    char self[4096];
    (void)argc;
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl <= 0) { snprintf(self, sizeof(self), "%s", argv[0]); }
    else self[sl] = 0;
    return run_bridge(self);
}
