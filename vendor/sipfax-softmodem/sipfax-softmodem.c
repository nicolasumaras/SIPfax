#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES
#include <spandsp.h>

#define FRAME_HEADER_BYTES 2
#define MAX_FRAME_BYTES 65535
#define DEFAULT_CONTROL_FD 3
#define DEFAULT_DATA_OUT "sipfax-softmodem-data.bin"
#define HDLC_FLAG 0x7e
#define HDLC_ESCAPE 0x7d
#define HDLC_ESCAPE_MASK 0x20
#define HDLC_FCS_INIT 0xffff
#define HDLC_FCS_GOOD 0xf0b8
#define HDLC_MAX_FRAME_BYTES 4096
#define HDLC_TX_QUEUE_BYTES 65536

typedef enum {
    CODEC_ULAW,
    CODEC_ALAW
} codec_kind_t;

typedef enum {
    MODULATION_V21,
    MODULATION_V22BIS
} modulation_kind_t;

typedef enum {
    START_MODE_V8,
    START_MODE_V21,
    START_MODE_V22BIS
} start_mode_t;

typedef struct {
    uint8_t buffer[HDLC_MAX_FRAME_BYTES];
    size_t length;
    bool in_frame;
    bool escaped;
} hdlc_rx_t;

typedef struct {
    uint8_t bytes[HDLC_TX_QUEUE_BYTES];
    size_t head;
    size_t tail;
    bool current_valid;
    uint8_t current_byte;
    int bit_index;
} hdlc_tx_t;

typedef struct {
    codec_kind_t codec;
    int payload_type;
    int clock_rate;
    int control_fd;
    FILE *data_out;
    v8_state_t *v8;
    fsk_rx_state_t *v21_rx;
    fsk_tx_state_t *v21_tx;
    v22bis_state_t *v22bis;
    int pty_master_fd;
    char pty_slave_path[128];
    hdlc_rx_t hdlc_rx;
    hdlc_tx_t hdlc_tx;
    modulation_kind_t modulation;
    start_mode_t start_mode;
    bool data_mode;
    unsigned int bit_accumulator;
    int bit_count;
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t decoded_bytes;
    uint64_t hdlc_frames_in;
    uint64_t hdlc_frames_out;
    uint64_t pty_bytes_in;
    uint64_t pty_bytes_out;
    int v8_status;
    int v8_modulations;
    const char *last_event;
} worker_t;

static int16_t ulaw_decode_table[256];
static int16_t alaw_decode_table[256];
static uint8_t ulaw_encode_table[65536];
static uint8_t alaw_encode_table[65536];

static void init_g711_tables(void);
static int parse_int_env(const char *name, int fallback);
static codec_kind_t parse_codec(void);
static int read_exact(int fd, uint8_t *buffer, size_t length);
static int write_frame(worker_t *worker, const uint8_t *payload, size_t length);
static int write_all(int fd, const uint8_t *buffer, size_t length);
static void decode_g711(worker_t *worker, int16_t *pcm, const uint8_t *payload, size_t length);
static void encode_g711(worker_t *worker, uint8_t *payload, const int16_t *pcm, size_t length);
static void emit_control(worker_t *worker, const char *event);
static const char *modulation_name(modulation_kind_t modulation);
static int modulation_baud(modulation_kind_t modulation);
static start_mode_t parse_start_mode(void);
static const char *start_mode_name(start_mode_t start_mode);
static modulation_kind_t parse_force_modulation(void);
static const char *v8_status_name(int status);
static uint16_t hdlc_fcs_update(uint16_t fcs, uint8_t byte);
static void hdlc_rx_byte(worker_t *worker, uint8_t byte);
static int hdlc_tx_enqueue_frame(worker_t *worker, const uint8_t *payload, size_t length);
static int hdlc_tx_enqueue_byte(worker_t *worker, uint8_t byte);
static int hdlc_tx_enqueue_escaped(worker_t *worker, uint8_t byte);
static int hdlc_tx_pop_byte(worker_t *worker, uint8_t *byte);
static int hdlc_tx_next_bit(void *user_data);
static void poll_pty(worker_t *worker);
static int open_pty(worker_t *worker);
static void close_pty(worker_t *worker);
static void enter_data_mode(worker_t *worker, modulation_kind_t modulation, const char *event);
static void v8_result(void *user_data, v8_parms_t *result);
static void put_v21_bit(void *user_data, int bit);
static void put_hdlc_bit(void *user_data, int bit);
static void v22bis_status(void *user_data, int status);
static int init_spandsp(worker_t *worker);
static void release_spandsp(worker_t *worker);

int main(void) {
    init_g711_tables();

    worker_t worker = {
        .codec = parse_codec(),
        .payload_type = parse_int_env("SIPFAX_MODEM_PAYLOAD_TYPE", 0),
        .clock_rate = parse_int_env("SIPFAX_MODEM_CLOCK_RATE", 8000),
        .control_fd = parse_int_env("SIPFAX_MODEM_CONTROL_FD", DEFAULT_CONTROL_FD),
        .pty_master_fd = -1,
        .modulation = MODULATION_V21,
        .start_mode = parse_start_mode(),
        .v8_status = -1,
        .v8_modulations = 0,
        .last_event = "starting"
    };

    const char *data_out_path = getenv("SIPFAX_MODEM_DATA_OUT");
    if (!data_out_path || data_out_path[0] == '\0') {
        data_out_path = DEFAULT_DATA_OUT;
    }
    worker.data_out = fopen(data_out_path, "ab");
    if (!worker.data_out) {
        fprintf(stderr, "sipfax-softmodem: open data output %s: %s\n", data_out_path, strerror(errno));
        return 1;
    }

    if (worker.clock_rate != 8000) {
        fprintf(stderr, "sipfax-softmodem: SIPFAX_MODEM_CLOCK_RATE must be 8000\n");
        fclose(worker.data_out);
        return 1;
    }

    if (worker.start_mode == START_MODE_V8 && init_spandsp(&worker) != 0) {
        fclose(worker.data_out);
        return 1;
    }

    emit_control(&worker, "started");
    if (parse_int_env("SIPFAX_MODEM_FORCE_DATA_MODE", 0) != 0) {
        enter_data_mode(&worker, parse_force_modulation(), "forced-data-mode");
    } else if (worker.start_mode == START_MODE_V22BIS) {
        enter_data_mode(&worker, MODULATION_V22BIS, "start-mode-v22bis");
    } else if (worker.start_mode == START_MODE_V21) {
        enter_data_mode(&worker, MODULATION_V21, "start-mode-v21");
    }

    for (;;) {
        uint8_t header[FRAME_HEADER_BYTES];
        int header_status = read_exact(STDIN_FILENO, header, sizeof(header));
        if (header_status == 0) {
            break;
        }
        if (header_status < 0) {
            fprintf(stderr, "sipfax-softmodem: read frame header: %s\n", strerror(errno));
            release_spandsp(&worker);
            fclose(worker.data_out);
            return 1;
        }

        size_t frame_len = ((size_t) header[0] << 8) | header[1];
        uint8_t payload[MAX_FRAME_BYTES];
        if (read_exact(STDIN_FILENO, payload, frame_len) <= 0) {
            fprintf(stderr, "sipfax-softmodem: truncated input frame\n");
            release_spandsp(&worker);
            fclose(worker.data_out);
            return 1;
        }

        int16_t pcm[MAX_FRAME_BYTES];
        uint8_t outbound[MAX_FRAME_BYTES];
        memset(outbound, worker.codec == CODEC_ALAW ? G711_ALAW_IDLE_OCTET : G711_ULAW_IDLE_OCTET, frame_len);

        decode_g711(&worker, pcm, payload, frame_len);
        worker.frames_in++;

        if (!worker.data_mode && worker.v8) {
            v8_rx(worker.v8, pcm, (int) frame_len);
            if (!worker.data_mode && worker.v8) {
                int generated = v8_tx(worker.v8, pcm, (int) frame_len);
                if (generated > 0) {
                    encode_g711(&worker, outbound, pcm, (size_t) generated);
                }
            }
        } else if (worker.modulation == MODULATION_V21 && worker.v21_rx) {
            poll_pty(&worker);
            fsk_rx(worker.v21_rx, pcm, (int) frame_len);
            if (worker.v21_tx) {
                int generated = fsk_tx(worker.v21_tx, pcm, (int) frame_len);
                if (generated > 0) {
                    encode_g711(&worker, outbound, pcm, (size_t) generated);
                }
            }
        } else if (worker.modulation == MODULATION_V22BIS && worker.v22bis) {
            poll_pty(&worker);
            v22bis_rx(worker.v22bis, pcm, (int) frame_len);
            int generated = v22bis_tx(worker.v22bis, pcm, (int) frame_len);
            if (generated > 0) {
                encode_g711(&worker, outbound, pcm, (size_t) generated);
            }
        }

        if (write_frame(&worker, outbound, frame_len) != 0) {
            release_spandsp(&worker);
            fclose(worker.data_out);
            return 1;
        }

    }

    emit_control(&worker, "eof");
    release_spandsp(&worker);
    fclose(worker.data_out);
    return 0;
}

static int parse_int_env(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0') {
        return fallback;
    }
    return (int) parsed;
}

static void init_g711_tables(void) {
    for (int index = 0; index < 256; index++) {
        ulaw_decode_table[index] = ulaw_to_linear((uint8_t) index);
        alaw_decode_table[index] = alaw_to_linear((uint8_t) index);
    }

    for (int sample = -32768; sample <= 32767; sample++) {
        uint16_t table_index = (uint16_t) (int16_t) sample;
        ulaw_encode_table[table_index] = linear_to_ulaw(sample);
        alaw_encode_table[table_index] = linear_to_alaw(sample);
        if (sample == 32767) {
            break;
        }
    }
}

static codec_kind_t parse_codec(void) {
    const char *codec = getenv("SIPFAX_MODEM_CODEC");
    const char *payload_type = getenv("SIPFAX_MODEM_PAYLOAD_TYPE");
    if ((codec && strcmp(codec, "PCMA") == 0) || (payload_type && strcmp(payload_type, "8") == 0)) {
        return CODEC_ALAW;
    }
    return CODEC_ULAW;
}

static int read_exact(int fd, uint8_t *buffer, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = read(fd, buffer + offset, length - offset);
        if (count == 0) {
            return offset == 0 ? 0 : -1;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += (size_t) count;
    }
    return 1;
}

static int write_frame(worker_t *worker, const uint8_t *payload, size_t length) {
    uint8_t header[FRAME_HEADER_BYTES] = {
        (uint8_t) ((length >> 8) & 0xff),
        (uint8_t) (length & 0xff)
    };
    if (fwrite(header, 1, sizeof(header), stdout) != sizeof(header) ||
        fwrite(payload, 1, length, stdout) != length ||
        fflush(stdout) != 0) {
        fprintf(stderr, "sipfax-softmodem: write stdout frame: %s\n", strerror(errno));
        return -1;
    }
    worker->frames_out++;
    return 0;
}

static int write_all(int fd, const uint8_t *buffer, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = write(fd, buffer + offset, length - offset);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += (size_t) count;
    }
    return 0;
}

static void decode_g711(worker_t *worker, int16_t *pcm, const uint8_t *payload, size_t length) {
    for (size_t index = 0; index < length; index++) {
        pcm[index] = worker->codec == CODEC_ALAW ? alaw_decode_table[payload[index]] : ulaw_decode_table[payload[index]];
    }
}

static void encode_g711(worker_t *worker, uint8_t *payload, const int16_t *pcm, size_t length) {
    for (size_t index = 0; index < length; index++) {
        uint16_t table_index = (uint16_t) pcm[index];
        payload[index] = worker->codec == CODEC_ALAW ? alaw_encode_table[table_index] : ulaw_encode_table[table_index];
    }
}

static void emit_control(worker_t *worker, const char *event) {
    worker->last_event = event;
    dprintf(worker->control_fd,
            "{\"state\":\"%s\",\"modulation\":\"%s\",\"baud\":%d,"
            "\"ber\":null,\"framesIn\":%llu,\"framesOut\":%llu,"
            "\"decodedBytes\":%llu,\"hdlcFramesIn\":%llu,\"hdlcFramesOut\":%llu,"
            "\"ptyBytesIn\":%llu,\"ptyBytesOut\":%llu,"
            "\"startMode\":\"%s\",\"v8Status\":",
            worker->data_mode ? "data-mode" : "negotiating",
            modulation_name(worker->modulation),
            modulation_baud(worker->modulation),
            (unsigned long long) worker->frames_in,
            (unsigned long long) worker->frames_out,
            (unsigned long long) worker->decoded_bytes,
            (unsigned long long) worker->hdlc_frames_in,
            (unsigned long long) worker->hdlc_frames_out,
            (unsigned long long) worker->pty_bytes_in,
            (unsigned long long) worker->pty_bytes_out,
            start_mode_name(worker->start_mode));
    if (worker->v8_status >= 0) {
        dprintf(worker->control_fd, "\"%s\"", v8_status_name(worker->v8_status));
    } else {
        dprintf(worker->control_fd, "null");
    }
    dprintf(worker->control_fd, ",\"v8StatusCode\":%d,\"v8Modulations\":%d,\"ptySlavePath\":",
            worker->v8_status,
            worker->v8_modulations);
    if (worker->pty_slave_path[0] != '\0') {
        dprintf(worker->control_fd, "\"%s\"", worker->pty_slave_path);
    } else {
        dprintf(worker->control_fd, "null");
    }
    dprintf(worker->control_fd, ",\"lastEvent\":\"%s\"}\n", event);
}

static const char *modulation_name(modulation_kind_t modulation) {
    return modulation == MODULATION_V22BIS ? "V.22bis" : "V.21";
}

static int modulation_baud(modulation_kind_t modulation) {
    return modulation == MODULATION_V22BIS ? 2400 : 300;
}

static start_mode_t parse_start_mode(void) {
    const char *value = getenv("SIPFAX_MODEM_START_MODE");
    if (!value || value[0] == '\0' || strcmp(value, "v8") == 0 || strcmp(value, "V.8") == 0 || strcmp(value, "V8") == 0) {
        return START_MODE_V8;
    }
    if (strcmp(value, "v22bis") == 0 || strcmp(value, "V.22bis") == 0 || strcmp(value, "V22bis") == 0) {
        return START_MODE_V22BIS;
    }
    if (strcmp(value, "v21") == 0 || strcmp(value, "V.21") == 0 || strcmp(value, "V21") == 0) {
        return START_MODE_V21;
    }
    return START_MODE_V8;
}

static const char *start_mode_name(start_mode_t start_mode) {
    if (start_mode == START_MODE_V22BIS) {
        return "v22bis";
    }
    if (start_mode == START_MODE_V21) {
        return "v21";
    }
    return "v8";
}

static modulation_kind_t parse_force_modulation(void) {
    const char *value = getenv("SIPFAX_MODEM_FORCE_MODULATION");
    if (value && (strcmp(value, "V.22bis") == 0 || strcmp(value, "V22bis") == 0 || strcmp(value, "v22bis") == 0)) {
        return MODULATION_V22BIS;
    }
    return MODULATION_V21;
}

static const char *v8_status_name(int status) {
    switch (status) {
    case V8_STATUS_V8_CALL:
        return "v8-call";
    case V8_STATUS_NON_V8_CALL:
        return "non-v8-call";
    case V8_STATUS_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

static uint16_t hdlc_fcs_update(uint16_t fcs, uint8_t byte) {
    fcs ^= byte;
    for (int bit = 0; bit < 8; bit++) {
        if (fcs & 1) {
            fcs = (uint16_t) ((fcs >> 1) ^ 0x8408);
        } else {
            fcs = (uint16_t) (fcs >> 1);
        }
    }
    return fcs;
}

static void hdlc_rx_byte(worker_t *worker, uint8_t byte) {
    hdlc_rx_t *rx = &worker->hdlc_rx;

    if (byte == HDLC_FLAG) {
        if (rx->in_frame && rx->length >= 2) {
            uint16_t fcs = HDLC_FCS_INIT;
            for (size_t index = 0; index < rx->length; index++) {
                fcs = hdlc_fcs_update(fcs, rx->buffer[index]);
            }
            if (fcs == HDLC_FCS_GOOD) {
                size_t payload_length = rx->length - 2;
                if (payload_length > 0) {
                    if (worker->pty_master_fd >= 0) {
                        if (write_all(worker->pty_master_fd, rx->buffer, payload_length) == 0) {
                            worker->pty_bytes_out += payload_length;
                        }
                    }
                    if (worker->data_out) {
                        fwrite(rx->buffer, 1, payload_length, worker->data_out);
                        fflush(worker->data_out);
                    }
                    worker->decoded_bytes += payload_length;
                }
                worker->hdlc_frames_in++;
                worker->last_event = "hdlc-frame-decoded";
            }
        }
        rx->in_frame = true;
        rx->escaped = false;
        rx->length = 0;
        return;
    }

    if (!rx->in_frame) {
        return;
    }

    if (byte == HDLC_ESCAPE) {
        rx->escaped = true;
        return;
    }

    if (rx->escaped) {
        byte ^= HDLC_ESCAPE_MASK;
        rx->escaped = false;
    }

    if (rx->length >= sizeof(rx->buffer)) {
        rx->in_frame = false;
        rx->escaped = false;
        rx->length = 0;
        worker->last_event = "hdlc-frame-too-large";
        return;
    }

    rx->buffer[rx->length++] = byte;
}

static int hdlc_tx_enqueue_byte(worker_t *worker, uint8_t byte) {
    hdlc_tx_t *tx = &worker->hdlc_tx;
    size_t next_tail = (tx->tail + 1) % sizeof(tx->bytes);
    if (next_tail == tx->head) {
        worker->last_event = "hdlc-tx-queue-full";
        return -1;
    }
    tx->bytes[tx->tail] = byte;
    tx->tail = next_tail;
    return 0;
}

static int hdlc_tx_enqueue_escaped(worker_t *worker, uint8_t byte) {
    if (byte == HDLC_FLAG || byte == HDLC_ESCAPE || byte < 0x20) {
        if (hdlc_tx_enqueue_byte(worker, HDLC_ESCAPE) != 0 ||
            hdlc_tx_enqueue_byte(worker, byte ^ HDLC_ESCAPE_MASK) != 0) {
            return -1;
        }
        return 0;
    }
    return hdlc_tx_enqueue_byte(worker, byte);
}

static int hdlc_tx_enqueue_frame(worker_t *worker, const uint8_t *payload, size_t length) {
    uint16_t fcs = HDLC_FCS_INIT;
    if (hdlc_tx_enqueue_byte(worker, HDLC_FLAG) != 0) {
        return -1;
    }
    for (size_t index = 0; index < length; index++) {
        fcs = hdlc_fcs_update(fcs, payload[index]);
        if (hdlc_tx_enqueue_escaped(worker, payload[index]) != 0) {
            return -1;
        }
    }
    fcs ^= 0xffff;
    if (hdlc_tx_enqueue_escaped(worker, (uint8_t) (fcs & 0xff)) != 0 ||
        hdlc_tx_enqueue_escaped(worker, (uint8_t) ((fcs >> 8) & 0xff)) != 0 ||
        hdlc_tx_enqueue_byte(worker, HDLC_FLAG) != 0) {
        return -1;
    }
    worker->hdlc_frames_out++;
    return 0;
}

static int hdlc_tx_pop_byte(worker_t *worker, uint8_t *byte) {
    hdlc_tx_t *tx = &worker->hdlc_tx;
    if (tx->head == tx->tail) {
        return 0;
    }
    *byte = tx->bytes[tx->head];
    tx->head = (tx->head + 1) % sizeof(tx->bytes);
    return 1;
}

static int hdlc_tx_next_bit(void *user_data) {
    worker_t *worker = (worker_t *) user_data;
    hdlc_tx_t *tx = &worker->hdlc_tx;

    if (!tx->current_valid) {
        if (hdlc_tx_pop_byte(worker, &tx->current_byte) <= 0) {
            tx->current_byte = HDLC_FLAG;
        }
        tx->current_valid = true;
        tx->bit_index = -1;
    }

    if (tx->bit_index < 0) {
        tx->bit_index = 0;
        return 0;
    }
    if (tx->bit_index < 8) {
        int bit = (tx->current_byte >> tx->bit_index) & 1;
        tx->bit_index++;
        return bit;
    }

    tx->current_valid = false;
    return 1;
}

static void poll_pty(worker_t *worker) {
    if (worker->pty_master_fd < 0) {
        return;
    }

    for (;;) {
        struct pollfd pfd = {
            .fd = worker->pty_master_fd,
            .events = POLLIN
        };
        int ready = poll(&pfd, 1, 0);
        if (ready <= 0 || !(pfd.revents & POLLIN)) {
            return;
        }

        uint8_t buffer[512];
        ssize_t count = read(worker->pty_master_fd, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            close_pty(worker);
            return;
        }
        if (count == 0) {
            close_pty(worker);
            return;
        }
        worker->pty_bytes_in += (uint64_t) count;
        if (hdlc_tx_enqueue_frame(worker, buffer, (size_t) count) != 0) {
            emit_control(worker, "hdlc-tx-queue-full");
            return;
        }
    }
}

static int open_pty(worker_t *worker) {
    if (worker->pty_master_fd >= 0) {
        return 0;
    }

    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "sipfax-softmodem: posix_openpt: %s\n", strerror(errno));
        return -1;
    }
    if (grantpt(fd) != 0 || unlockpt(fd) != 0) {
        fprintf(stderr, "sipfax-softmodem: unlock pty: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    char *slave = ptsname(fd);
    if (!slave) {
        fprintf(stderr, "sipfax-softmodem: ptsname: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct termios term;
    if (tcgetattr(fd, &term) == 0) {
        term.c_iflag = 0;
        term.c_oflag = 0;
        term.c_lflag = 0;
        term.c_cflag = (term.c_cflag & ~(CSIZE | PARENB)) | CS8;
        term.c_cc[VMIN] = 1;
        term.c_cc[VTIME] = 0;
        (void) tcsetattr(fd, TCSANOW, &term);
    }

    worker->pty_master_fd = fd;
    snprintf(worker->pty_slave_path, sizeof(worker->pty_slave_path), "%s", slave);
    emit_control(worker, "pty-opened");
    return 0;
}

static void close_pty(worker_t *worker) {
    if (worker->pty_master_fd < 0) {
        return;
    }
    close(worker->pty_master_fd);
    worker->pty_master_fd = -1;
    worker->pty_slave_path[0] = '\0';
    emit_control(worker, "pty-closed");
}

static void enter_data_mode(worker_t *worker, modulation_kind_t modulation, const char *event) {
    if (worker->data_mode) {
        return;
    }
    worker->data_mode = true;
    worker->modulation = modulation;
    worker->last_event = event;
    if (open_pty(worker) != 0) {
        return;
    }
    if (worker->v8) {
        v8_free(worker->v8);
        worker->v8 = NULL;
    }
    if (worker->modulation == MODULATION_V22BIS) {
        worker->v22bis = v22bis_init(
            NULL,
            2400,
            V22BIS_GUARD_TONE_NONE,
            false,
            hdlc_tx_next_bit,
            worker,
            put_hdlc_bit,
            worker
        );
        if (!worker->v22bis) {
            fprintf(stderr, "sipfax-softmodem: v22bis_init failed\n");
            return;
        }
        v22bis_rx_signal_cutoff(worker->v22bis, -45.0f);
        v22bis_set_modem_status_handler(worker->v22bis, v22bis_status, worker);
    } else {
        worker->v21_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V21CH1], FSK_FRAME_MODE_ASYNC, put_v21_bit, worker);
        if (!worker->v21_rx) {
            fprintf(stderr, "sipfax-softmodem: fsk_rx_init failed\n");
            return;
        }
        worker->v21_tx = fsk_tx_init(NULL, &preset_fsk_specs[FSK_V21CH2], hdlc_tx_next_bit, worker);
        if (!worker->v21_tx) {
            fprintf(stderr, "sipfax-softmodem: fsk_tx_init failed\n");
            return;
        }
        fsk_rx_signal_cutoff(worker->v21_rx, -45.0f);
    }
    emit_control(worker, event);
}

static void v8_result(void *user_data, v8_parms_t *result) {
    worker_t *worker = (worker_t *) user_data;
    worker->v8_status = result->status;
    worker->v8_modulations = result->modulations;
    if (result->status == V8_STATUS_V8_CALL) {
        if ((result->modulations & V8_MOD_V22) != 0) {
            enter_data_mode(worker, MODULATION_V22BIS, "v8-v22bis-selected");
        } else {
            enter_data_mode(worker, MODULATION_V21, "v8-v21-selected");
        }
    } else if (result->status == V8_STATUS_NON_V8_CALL) {
        enter_data_mode(worker, MODULATION_V21, "non-v8-v21-fallback");
    } else if (result->status == V8_STATUS_FAILED) {
        enter_data_mode(worker, MODULATION_V21, "v8-failed-v21-fallback");
    }
}

static void put_v21_bit(void *user_data, int bit) {
    put_hdlc_bit(user_data, bit);
}

static void put_hdlc_bit(void *user_data, int bit) {
    worker_t *worker = (worker_t *) user_data;
    if (bit < 0) {
        return;
    }

    worker->bit_accumulator |= (unsigned int) (bit & 1) << worker->bit_count;
    worker->bit_count++;

    if (worker->bit_count == 8) {
        uint8_t byte = (uint8_t) (worker->bit_accumulator & 0xff);
        hdlc_rx_byte(worker, byte);
        worker->bit_accumulator = 0;
        worker->bit_count = 0;
        worker->last_event = worker->modulation == MODULATION_V22BIS ? "v22bis-byte-decoded" : "v21-byte-decoded";
    }
}

static void v22bis_status(void *user_data, int status) {
    worker_t *worker = (worker_t *) user_data;
    if (status < 0) {
        worker->last_event = "v22bis-carrier-down";
    } else {
        worker->last_event = "v22bis-carrier-up";
    }
}

static int init_spandsp(worker_t *worker) {
    v8_parms_t parms;
    memset(&parms, 0, sizeof(parms));
    parms.modem_connect_tone = MODEM_CONNECT_TONES_ANSAM_PR;
    parms.send_ci = false;
    parms.call_function = V8_CALL_V_SERIES;
    parms.modulations = V8_MOD_V21 | V8_MOD_V22;
    parms.protocol = V8_PROTOCOL_NONE;

    worker->v8 = v8_init(NULL, false, &parms, v8_result, worker);
    if (!worker->v8) {
        fprintf(stderr, "sipfax-softmodem: v8_init failed\n");
        return -1;
    }
    return 0;
}

static void release_spandsp(worker_t *worker) {
    if (worker->v8) {
        v8_free(worker->v8);
        worker->v8 = NULL;
    }
    if (worker->v21_rx) {
        fsk_rx_free(worker->v21_rx);
        worker->v21_rx = NULL;
    }
    if (worker->v21_tx) {
        fsk_tx_free(worker->v21_tx);
        worker->v21_tx = NULL;
    }
    if (worker->v22bis) {
        v22bis_free(worker->v22bis);
        worker->v22bis = NULL;
    }
    close_pty(worker);
}
