/*
 * SIPfax engine driver for linmodem: audio over stdio as G.711 (8 kHz, native,
 * no resampling), data over a pty for pppd, control on fd 3 — the same contract
 * as the slmodem bridge. Answers the call (V.8 -> highest negotiated modulation,
 * incl. the V.90 server path being developed).
 */
#define _XOPEN_SOURCE 600
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <termios.h>
#include "lm.h"
#include "v90lineecho.h"
#include "v90echodelay.h"

extern struct sm_hw_info sm_hw_null;
extern char *sm_states_str[];

static const char *st_name(int s){ return (s>=0 && s<256 && sm_states_str[s]) ? sm_states_str[s] : "?"; }

/* ---- G.711 (CCITT reference) ---- */
#define G711_BIAS 0x84
static int g_alaw = 0;
static s16 ulaw2lin(u8 u){ u=~u; int t=((u&0x0f)<<3)+G711_BIAS; t<<=((unsigned)u&0x70)>>4; return (s16)((u&0x80)?(G711_BIAS-t):(t-G711_BIAS)); }
static u8 lin2ulaw(s16 pcm) {
    /* Keep magnitude in int: negating INT16_MIN must not wrap. */
    int sign = pcm < 0 ? 0x80 : 0;
    int mag = pcm < 0 ? -(int)pcm : (int)pcm;
    if (mag > 32635) mag = 32635;
    mag += G711_BIAS;
    int exp = 7;
    for (int mask = 0x4000; exp > 0 && !(mag & mask); mask >>= 1) exp--;
    return (u8)~(sign | (exp << 4) | ((mag >> (exp + 3)) & 0x0f));
}
static s16 alaw2lin(u8 a){ a^=0x55; int t=(a&0x0f)<<4,seg=((unsigned)a&0x70)>>4; if(seg==0)t+=8; else if(seg==1)t+=0x108; else {t+=0x108;t<<=seg-1;} return (s16)((a&0x80)?t:-t); }
static u8 lin2alaw(s16 pcm){ int s=((~pcm)>>8)&0x80; if(!s)pcm=(s16)-pcm; if(pcm>0x7fff)pcm=0x7fff; u8 a; if(pcm<256)a=(u8)(pcm>>4); else {int e=7;for(int k=0x4000;e>1&&!(pcm&k);k>>=1)e--; int man=(pcm>>(e+3))&0x0f; a=(u8)((e<<4)|man);} return (u8)((a^0x55)|s); }
static s16 g711_dec(u8 v){ return g_alaw?alaw2lin(v):ulaw2lin(v); }
static u8  g711_enc(s16 v){ return g_alaw?lin2alaw(v):lin2ulaw(v); }

/* ---- exact framed I/O (2-byte BE length + payload), matching SIPfax ---- */
static int read_full(int fd,void*b,int n){ unsigned char*p=b; int g=0; while(g<n){ int r=read(fd,p+g,n-g); if(r==0)return 0; if(r<0){if(errno==EINTR)continue;return -1;} g+=r; } return 1; }
static int write_full(int fd,const void*b,int n){ const unsigned char*p=b; int w=0; while(w<n){ int r=write(fd,p+w,n-w); if(r<0){if(errno==EINTR)continue;return -1;} w+=r; } return 1; }
static int read_frame(int fd,u8*pay,int cap,int*len){ u8 h[2]; int r=read_full(fd,h,2); if(r<=0)return r; int n=(h[0]<<8)|h[1]; if(n>cap)return -1; r=read_full(fd,pay,n); if(r<=0)return -1; *len=n; return 1; }
static int write_frame(int fd,const u8*pay,int n){ u8 h[2]={(u8)(n>>8),(u8)(n&0xff)}; if(write_full(fd,h,2)<0)return -1; return write_full(fd,pay,n); }

void pipe_modem(void)
{
    struct sm_state sm1, *dce = &sm1;
    V90LineEcho line_echo={0};
    V90EchoDelay echo_delay={0};
    int auto_echo=0;
    s16 in_buf[2048], out_buf[2048];
    u8 pay[4096], g711out[2048], data[1024];
    int pty, len, i, n, last_state = -1, last_sm = -1, frames = 0;
    int pty_reported = 0;
    long long rx_acc = 0; int rx_cnt = 0;
    FILE *cap = NULL, *txcap = NULL;
    /* Legacy DSP diagnostics use printf. Reserve the original stdout for
       framed audio, then send diagnostics to stderr before any DSP init. */
    int audio_fd = fcntl(STDOUT_FILENO, F_DUPFD, 4);
    if (audio_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        perror("linmodem audio fd");
        if (audio_fd >= 0) close(audio_fd);
        return;
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *codec = getenv("SIPFAX_MODEM_CODEC");
    if (codec && (!strcasecmp(codec,"PCMA")||!strcasecmp(codec,"alaw"))) g_alaw = 1;

    /* optional: dump decoded upstream (modem -> us) S16LE @8k for offline analysis */
    char capbuf[256], txbuf[256];
    const char *cappath = getenv("SIPFAX_LINMODEM_CAPTURE");
    if (cappath) { snprintf(capbuf,sizeof(capbuf),"%s.%d",cappath,(int)getpid()); cap = fopen(capbuf, "wb"); }
    /* optional: dump OUR transmitted S16LE (us -> modem) to verify Phase 3 S/PP/TRN */
    const char *txpath = getenv("SIPFAX_LINMODEM_TXCAP");
    if (txpath) { snprintf(txbuf,sizeof(txbuf),"%s.%d",txpath,(int)getpid()); txcap = fopen(txbuf, "wb"); }

    pty = open("/dev/ptmx", O_RDWR);
    if (pty < 0) { perror("/dev/ptmx"); close(audio_fd); return; }
    grantpt(pty); unlockpt(pty);
    fcntl(pty, F_SETFL, O_NONBLOCK);
    /* A verified PPP frame may arrive before pppd opens the slave. Disable
       terminal echo/translations now so it cannot loop back as modem data. */
    struct termios tty;
    if(tcgetattr(pty,&tty)<0){perror("pty attributes");close(pty);close(audio_fd);return;}
    cfmakeraw(&tty);
    if(tcsetattr(pty,TCSANOW,&tty)<0){perror("pty raw mode");close(pty);close(audio_fd);return;}
    dprintf(3, "{\"event\":\"started\",\"engine\":\"linmodem\"}\n");
    fprintf(stderr, "[linmodem] pipe engine up (codec=%s cap=%s)\n", g_alaw?"alaw":"ulaw", cappath?cappath:"-"); fflush(stderr);

    lm_init(dce, &sm_hw_null, "v90");
    lm_start_receive(dce);         /* sets SM_RECEIVE -> immediate V.8 answer + ANSam */
    /* NOTE: do NOT force SM_TEST_RING here — that is the simulation-only ring path
       (5s ring_timer) and delays ANSam by 5s, desyncing the real modem's V.8. */

    /* Opt-in echo cancellation: measured delay or causal acquisition. */
    const char *echo_option=getenv("SIPFAX_V90_LINE_ECHO");
    if(echo_option && !strcmp(echo_option,"auto")) {
        auto_echo=1;v90_echo_delay_init(&echo_delay);
        fprintf(stderr,"[linmodem] experimental line echo: acquiring delay\n");
    } else if(echo_option && !strcmp(echo_option,"1")) {
        v90_line_echo_init(&line_echo,1428);
        fprintf(stderr,"[linmodem] experimental line echo: delay=1428 taps=65 step=0.0005\n");
    }
    for (;;) {
        if (read_frame(0, pay, sizeof(pay), &len) <= 0) break;   /* RTP closed */
        if (len > (int)(sizeof(in_buf)/2)) len = sizeof(in_buf)/2;
        for (i = 0; i < len; i++) in_buf[i] = g711_dec(pay[i]);
        if (cap) fwrite(in_buf, 2, len, cap);
        for (i = 0; i < len; i++) { int a = in_buf[i]<0?-in_buf[i]:in_buf[i]; rx_acc += a; rx_cnt++; }

        /* pty -> modem tx data */
        /* Leave excess bytes in the PTY so the kernel applies backpressure.
           sm_put_bit drops silently once its bounded FIFO is full. */
        int room = dce->tx_fifo.max_size - sm_size(&dce->tx_fifo);
        if (room > (int)sizeof(data)) room = sizeof(data);
        /* CTS equivalent for the virtual DTE: leave bytes in the PTY while
           V.90 retrains, preserving bounded backpressure and the PPP process. */
        if (pty_reported && dce->state == SM_V90 &&
            (!dce->u.v90_state.startup.phase4_active ||
             dce->u.v90_state.startup.phase4.stage != 4)) room = 0;
        n = room > 0 ? read(pty, data, room) : 0;
        for (i = 0; i < n; i++) sm_put_bit(&dce->tx_fifo, data[i]);

        if(line_echo.enabled || auto_echo)for(i=0;i<len;++i){
            if(auto_echo)v90_echo_delay_rx(&echo_delay,in_buf[i]);
            in_buf[i]=v90_line_echo_rx(&line_echo,in_buf[i]);
        }
        sm_process(dce, out_buf, in_buf, len);
        if(auto_echo && !echo_delay.locked && v90_echo_delay_step(&echo_delay,&line_echo))
            fprintf(stderr,"[linmodem] line echo acquired: delay=%u sample=%llu\n",echo_delay.delay,(unsigned long long)echo_delay.lock_sample);
        if(line_echo.enabled || auto_echo)for(i=0;i<len;++i)v90_line_echo_tx(&line_echo,out_buf[i]);

        /* modem rx data -> pty */
        {
            struct sm_fifo *f = &dce->rx_fifo;
            int size = f->eptr - f->rptr;
            if (size > f->size) size = f->size;
            if (size > 0) {
                int w = write(pty, f->rptr, size);
                if (w > 0) { f->rptr += w; f->size -= w; if (f->rptr == f->eptr) f->rptr = f->sptr; }
            }
        }

        if (txcap) fwrite(out_buf, 2, len, txcap);
        for (i = 0; i < len; i++) g711out[i] = g711_enc(out_buf[i]);
        if (write_frame(audio_fd, g711out, len) < 0) break;

        /* log every internal state transition (V.8 -> datapump) to stderr */
        if (dce->state != last_sm) {
            fprintf(stderr, "[linmodem] state %s -> %s\n", st_name(last_sm), st_name(dce->state));
            fflush(stderr);
            last_sm = dce->state;
        }

        /* periodic RX level (confirms we hear the modem's upstream) ~1/s */
        if (++frames % 50 == 0) {
            int rms = rx_cnt ? (int)(rx_acc / rx_cnt) : 0;
            fprintf(stderr, "[linmodem] t=%ds rx_avg=%d state=%s\n", frames/50, rms, st_name(dce->state));
            fflush(stderr);
            rx_acc = 0; rx_cnt = 0;
        }

        /* report connection state changes on fd 3 */
        {
            int st = lm_get_state(dce);
            if (st != last_state) {
                last_state = st;
                if (st == LM_STATE_CONNECTED && !pty_reported) {
                    pty_reported = 1;
                    fprintf(stderr, "[linmodem] CONNECTED -> pty %s\n", ptsname(pty)); fflush(stderr);
                    dprintf(3, "{\"event\":\"pty-opened\",\"slavePath\":\"%s\",\"engine\":\"linmodem\"}\n", ptsname(pty));
                }
            }
        }
    }
    if (cap) fclose(cap);
    if (txcap) fclose(txcap);
    fprintf(stderr, "[linmodem] call ended after %d frames, final state %s\n", frames, st_name(last_sm)); fflush(stderr);
    close(pty);
    close(audio_fd);
}
