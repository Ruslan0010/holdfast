/*
 * packet.h — the holdfast wire format. Normative text is docs/PROTOCOL.md;
 * this header and the Python decoder in server/holdfast/protocol.py are two
 * independent implementations of it.
 *
 * Three message types, all carried in single UDP datagrams of at most 256
 * bytes, all ending in a CRC-32 over everything before it:
 *
 *   DATA       station -> server   a run of samples with its sequence number
 *   HEARTBEAT  station -> server   keeps the NAT mapping alive; carries the
 *                                  retention window so the server knows what
 *                                  is still recoverable
 *   GAPS       server  -> station  "send me these sequence ranges again"
 *
 * 256 bytes is the ring buffer slot size. It is small enough to fit a single
 * fragment on any link (IPv6 minimum MTU is 1280, most satellite modems
 * accept 512) and large enough for three Steim2 frames, which at 100 Hz on a
 * quiet signal is around three seconds of data per packet.
 *
 * A decoded DATA packet's payload pointer refers into the caller's buffer;
 * nothing is copied.
 */
#ifndef PACKET_H
#define PACKET_H

#include <stddef.h>
#include <stdint.h>

#define PKT_MAX_LEN 256u
#define PKT_VERSION 1u
#define PKT_CRC_LEN 4u

#define PKT_DATA_HDR_LEN     36u
#define PKT_DATA_MAX_PAYLOAD (PKT_MAX_LEN - PKT_DATA_HDR_LEN - PKT_CRC_LEN) /* 216 */
#define PKT_HEARTBEAT_LEN    36u
#define PKT_GAPS_HDR_LEN     12u
#define PKT_GAPS_RANGE_LEN   16u
#define PKT_GAPS_MAX_RANGES  12u
#define PKT_GAPS_MAX_LEN \
    (PKT_GAPS_HDR_LEN + PKT_GAPS_MAX_RANGES * PKT_GAPS_RANGE_LEN + PKT_CRC_LEN)

typedef enum {
    PKT_DATA      = 1,
    PKT_HEARTBEAT = 2,
    PKT_GAPS      = 3,
} pkt_type_t;

/* Sample encoding, DATA flags bits 2-3. */
#define PKT_ENC_RAW    0 /* int32 samples, big-endian, 4 bytes each */
#define PKT_ENC_STEIM2 1 /* Steim2 frames, 64 bytes each, see steim2.h */

/* Clock quality, DATA and HEARTBEAT flags bits 0-1. */
#define PKT_CLOCK_FREE     0 /* free-running oscillator, timestamp is a guess */
#define PKT_CLOCK_HOLDOVER 1 /* was disciplined, reference lost, drifting */
#define PKT_CLOCK_LOCKED   2 /* disciplined to PPS or NTP */

/* Return codes. */
#define PKT_OK           0
#define PKT_ERR_ARG     (-1) /* NULL where a pointer was required */
#define PKT_ERR_SHORT   (-2) /* buffer shorter than the minimum for its type */
#define PKT_ERR_MAGIC   (-3) /* not a holdfast packet */
#define PKT_ERR_VERSION (-4) /* protocol version we do not speak */
#define PKT_ERR_TYPE    (-5) /* unknown message type */
#define PKT_ERR_LEN     (-6) /* length fields disagree with the buffer */
#define PKT_ERR_CRC     (-7) /* checksum mismatch: corrupted in transit */
#define PKT_ERR_FIELD   (-8) /* a field holds a value outside its domain */
#define PKT_ERR_SIZE    (-9) /* output buffer too small */

typedef struct {
    uint32_t       station_id;
    uint16_t       stream_id;
    uint8_t        clock_quality;    /* PKT_CLOCK_* */
    uint8_t        encoding;         /* PKT_ENC_* */
    uint64_t       seq;
    uint64_t       t0_ns;            /* time of the first sample, Unix epoch */
    uint32_t       sample_period_us; /* 10000 for 100 Hz */
    uint16_t       sample_count;
    uint16_t       payload_len;
    const uint8_t *payload;
} pkt_data_t;

typedef struct {
    uint32_t station_id;
    uint64_t next_seq;   /* the next DATA packet will carry this seq */
    uint64_t oldest_seq; /* everything below this is unrecoverable */
    uint32_t uptime_s;
    uint8_t  clock_quality;
} pkt_heartbeat_t;

typedef struct {
    uint64_t from; /* inclusive */
    uint64_t to;   /* inclusive */
} pkt_range_t;

typedef struct {
    uint32_t    station_id;
    uint8_t     count;
    pkt_range_t ranges[PKT_GAPS_MAX_RANGES];
} pkt_gaps_t;

typedef struct {
    pkt_type_t type;
    union {
        pkt_data_t      data;
        pkt_heartbeat_t heartbeat;
        pkt_gaps_t      gaps;
    } u;
} pkt_t;

/* Encoders write a complete datagram including CRC into out and set *out_len.
 * Return PKT_OK, PKT_ERR_ARG, PKT_ERR_FIELD, or PKT_ERR_SIZE if cap is too
 * small (PKT_MAX_LEN is always enough). */
int pkt_encode_data(const pkt_data_t *d, uint8_t *out, size_t cap,
                    size_t *out_len);
int pkt_encode_heartbeat(const pkt_heartbeat_t *h, uint8_t *out, size_t cap,
                         size_t *out_len);
int pkt_encode_gaps(const pkt_gaps_t *g, uint8_t *out, size_t cap,
                    size_t *out_len);

/* Decode one datagram. The buffer must hold exactly one message: trailing
 * bytes are a length error, because a UDP datagram is a whole message and
 * anything else is corruption. On PKT_OK, out->type says which union member
 * is valid. */
int pkt_decode(const uint8_t *buf, size_t len, pkt_t *out);

/* Human-readable name for a return code, for logs. */
const char *pkt_strerror(int rc);

#endif /* PACKET_H */
