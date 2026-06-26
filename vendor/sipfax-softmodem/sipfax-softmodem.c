#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SPANDSP_EXPOSE_INTERNAL_STRUCTURES
#include <spandsp.h>

#define FRAME_HEADER_BYTES 2
#define MAX_FRAME_BYTES 65535
#define DEFAULT_CONTROL_FD 3
#define DEFAULT_DATA_OUT "sipfax-softmodem-data.bin"

typedef enum {
    CODEC_ULAW,
    CODEC_ALAW
} codec_kind_t;

typedef struct {
    codec_kind_t codec;
    int payload_type;
    int clock_rate;
    int control_fd;
    FILE *data_out;
    v8_state_t *v8;
    fsk_rx_state_t *v21_rx;
    bool data_mode;
    unsigned int bit_accumulator;
    int bit_count;
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t decoded_bytes;
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
static void decode_g711(worker_t *worker, int16_t *pcm, const uint8_t *payload, size_t length);
static void encode_g711(worker_t *worker, uint8_t *payload, const int16_t *pcm, size_t length);
static void emit_control(worker_t *worker, const char *event);
static void enter_data_mode(worker_t *worker, const char *event);
static void v8_result(void *user_data, v8_parms_t *result);
static void put_v21_bit(void *user_data, int bit);
static int init_spandsp(worker_t *worker);
static void release_spandsp(worker_t *worker);

int main(void) {
    init_g711_tables();

    worker_t worker = {
        .codec = parse_codec(),
        .payload_type = parse_int_env("SIPFAX_MODEM_PAYLOAD_TYPE", 0),
        .clock_rate = parse_int_env("SIPFAX_MODEM_CLOCK_RATE", 8000),
        .control_fd = parse_int_env("SIPFAX_MODEM_CONTROL_FD", DEFAULT_CONTROL_FD),
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

    if (init_spandsp(&worker) != 0) {
        fclose(worker.data_out);
        return 1;
    }

    emit_control(&worker, "started");

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
        } else if (worker.v21_rx) {
            fsk_rx(worker.v21_rx, pcm, (int) frame_len);
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
    dprintf(
        worker->control_fd,
        "{\"state\":\"%s\",\"modulation\":\"V.21\",\"baud\":300,"
        "\"ber\":null,\"framesIn\":%llu,\"framesOut\":%llu,"
        "\"decodedBytes\":%llu,\"lastEvent\":\"%s\"}\n",
        worker->data_mode ? "data-mode" : "negotiating",
        (unsigned long long) worker->frames_in,
        (unsigned long long) worker->frames_out,
        (unsigned long long) worker->decoded_bytes,
        event
    );
}

static void enter_data_mode(worker_t *worker, const char *event) {
    if (worker->data_mode) {
        return;
    }
    worker->data_mode = true;
    worker->last_event = event;
    if (worker->v8) {
        v8_free(worker->v8);
        worker->v8 = NULL;
    }
    worker->v21_rx = fsk_rx_init(NULL, &preset_fsk_specs[FSK_V21CH1], FSK_FRAME_MODE_ASYNC, put_v21_bit, worker);
    if (!worker->v21_rx) {
        fprintf(stderr, "sipfax-softmodem: fsk_rx_init failed\n");
        return;
    }
    fsk_rx_signal_cutoff(worker->v21_rx, -45.0f);
    emit_control(worker, event);
}

static void v8_result(void *user_data, v8_parms_t *result) {
    worker_t *worker = (worker_t *) user_data;
    if (result->status == V8_STATUS_V8_CALL || result->status == V8_STATUS_NON_V8_CALL) {
        enter_data_mode(worker, result->status == V8_STATUS_V8_CALL ? "v8-v21-selected" : "non-v8-v21-fallback");
    } else if (result->status == V8_STATUS_FAILED) {
        enter_data_mode(worker, "v8-failed-v21-fallback");
    }
}

static void put_v21_bit(void *user_data, int bit) {
    worker_t *worker = (worker_t *) user_data;
    if (bit < 0) {
        return;
    }

    worker->bit_accumulator |= (unsigned int) (bit & 1) << worker->bit_count;
    worker->bit_count++;

    if (worker->bit_count == 8) {
        uint8_t byte = (uint8_t) (worker->bit_accumulator & 0xff);
        fwrite(&byte, 1, 1, worker->data_out);
        fflush(worker->data_out);
        worker->decoded_bytes++;
        worker->bit_accumulator = 0;
        worker->bit_count = 0;
        worker->last_event = "v21-byte-decoded";
    }
}

static int init_spandsp(worker_t *worker) {
    v8_parms_t parms;
    memset(&parms, 0, sizeof(parms));
    parms.modem_connect_tone = true;
    parms.send_ci = false;
    parms.call_function = V8_CALL_V_SERIES;
    parms.modulations = V8_MOD_V21;
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
}
