/*
 * packet.c — encode and decode holdfast datagrams. See packet.h and
 * docs/PROTOCOL.md for the byte layout.
 *
 * Decoding order is: cheapest check first. Magic and version cost one byte
 * each, the length fields cost a comparison, and only then is the CRC
 * computed over the whole datagram. On a device that receives junk from a
 * misconfigured peer this keeps the cost of rejecting it near zero.
 */

#include "packet.h"

#include "crc32.h"
#include "wire.h"

#include <string.h>

#define MAGIC0 'H'
#define MAGIC1 'F'

/* DATA flags byte: bits 0-1 clock quality, bits 2-3 encoding. */
#define FLAG_CLOCK_MASK 0x03u
#define FLAG_ENC_SHIFT  2
#define FLAG_ENC_MASK   0x0Cu

/* Field checks shared by the encoder and the decoder, so that whatever one
 * side refuses to produce the other side refuses to accept. */
static int data_fields_valid(const pkt_data_t *d)
{
    if (d->clock_quality > PKT_CLOCK_LOCKED)
        return 0;
    if (d->encoding > PKT_ENC_STEIM2)
        return 0;
    if (d->payload_len > PKT_DATA_MAX_PAYLOAD)
        return 0;
    if (d->encoding == PKT_ENC_RAW && d->payload_len != d->sample_count * 4u)
        return 0;
    if (d->encoding == PKT_ENC_STEIM2) {
        if (d->payload_len % 64u != 0)
            return 0;
        if ((d->payload_len == 0) != (d->sample_count == 0))
            return 0;
    }
    return 1;
}

static void write_common(uint8_t *out, pkt_type_t type)
{
    out[0] = MAGIC0;
    out[1] = MAGIC1;
    out[2] = PKT_VERSION;
    out[3] = (uint8_t)type;
}

static void write_crc(uint8_t *out, size_t total)
{
    wr_u32(out + total - PKT_CRC_LEN, crc32_compute(out, total - PKT_CRC_LEN));
}

static int crc_ok(const uint8_t *buf, size_t total)
{
    return rd_u32(buf + total - PKT_CRC_LEN) ==
           crc32_compute(buf, total - PKT_CRC_LEN);
}

int pkt_encode_data(const pkt_data_t *d, uint8_t *out, size_t cap,
                    size_t *out_len)
{
    if (d == NULL || out == NULL || out_len == NULL)
        return PKT_ERR_ARG;
    if (d->payload == NULL && d->payload_len > 0)
        return PKT_ERR_ARG;
    if (!data_fields_valid(d))
        return PKT_ERR_FIELD;

    const size_t total = PKT_DATA_HDR_LEN + d->payload_len + PKT_CRC_LEN;
    if (cap < total)
        return PKT_ERR_SIZE;

    write_common(out, PKT_DATA);
    wr_u32(out + 4, d->station_id);
    wr_u16(out + 8, d->stream_id);
    out[10] = (uint8_t)((d->clock_quality & FLAG_CLOCK_MASK) |
                        ((unsigned)d->encoding << FLAG_ENC_SHIFT));
    out[11] = 0;
    wr_u64(out + 12, d->seq);
    wr_u64(out + 20, d->t0_ns);
    wr_u32(out + 28, d->sample_period_us);
    wr_u16(out + 32, d->sample_count);
    wr_u16(out + 34, d->payload_len);
    if (d->payload_len > 0)
        memcpy(out + PKT_DATA_HDR_LEN, d->payload, d->payload_len);
    write_crc(out, total);

    *out_len = total;
    return PKT_OK;
}

int pkt_encode_heartbeat(const pkt_heartbeat_t *h, uint8_t *out, size_t cap,
                         size_t *out_len)
{
    if (h == NULL || out == NULL || out_len == NULL)
        return PKT_ERR_ARG;
    if (h->clock_quality > PKT_CLOCK_LOCKED)
        return PKT_ERR_FIELD;
    if (h->oldest_seq > h->next_seq)
        return PKT_ERR_FIELD;
    if (cap < PKT_HEARTBEAT_LEN)
        return PKT_ERR_SIZE;

    write_common(out, PKT_HEARTBEAT);
    wr_u32(out + 4, h->station_id);
    wr_u64(out + 8, h->next_seq);
    wr_u64(out + 16, h->oldest_seq);
    wr_u32(out + 24, h->uptime_s);
    out[28] = (uint8_t)(h->clock_quality & FLAG_CLOCK_MASK);
    out[29] = 0;
    out[30] = 0;
    out[31] = 0;
    write_crc(out, PKT_HEARTBEAT_LEN);

    *out_len = PKT_HEARTBEAT_LEN;
    return PKT_OK;
}

int pkt_encode_gaps(const pkt_gaps_t *g, uint8_t *out, size_t cap,
                    size_t *out_len)
{
    if (g == NULL || out == NULL || out_len == NULL)
        return PKT_ERR_ARG;
    if (g->count > PKT_GAPS_MAX_RANGES)
        return PKT_ERR_FIELD;
    for (size_t i = 0; i < g->count; i++)
        if (g->ranges[i].from > g->ranges[i].to)
            return PKT_ERR_FIELD;

    const size_t total =
        PKT_GAPS_HDR_LEN + (size_t)g->count * PKT_GAPS_RANGE_LEN + PKT_CRC_LEN;
    if (cap < total)
        return PKT_ERR_SIZE;

    write_common(out, PKT_GAPS);
    wr_u32(out + 4, g->station_id);
    out[8]  = g->count;
    out[9]  = 0;
    out[10] = 0;
    out[11] = 0;
    for (size_t i = 0; i < g->count; i++) {
        uint8_t *r = out + PKT_GAPS_HDR_LEN + i * PKT_GAPS_RANGE_LEN;
        wr_u64(r, g->ranges[i].from);
        wr_u64(r + 8, g->ranges[i].to);
    }
    write_crc(out, total);

    *out_len = total;
    return PKT_OK;
}

static int decode_data(const uint8_t *buf, size_t len, pkt_data_t *d)
{
    if (len < PKT_DATA_HDR_LEN + PKT_CRC_LEN)
        return PKT_ERR_SHORT;

    const uint16_t payload_len = rd_u16(buf + 34);
    if (payload_len > PKT_DATA_MAX_PAYLOAD)
        return PKT_ERR_LEN;
    if (len != PKT_DATA_HDR_LEN + (size_t)payload_len + PKT_CRC_LEN)
        return PKT_ERR_LEN;
    if (!crc_ok(buf, len))
        return PKT_ERR_CRC;

    d->station_id       = rd_u32(buf + 4);
    d->stream_id        = rd_u16(buf + 8);
    d->clock_quality    = (uint8_t)(buf[10] & FLAG_CLOCK_MASK);
    d->encoding         = (uint8_t)((buf[10] & FLAG_ENC_MASK) >> FLAG_ENC_SHIFT);
    d->seq              = rd_u64(buf + 12);
    d->t0_ns            = rd_u64(buf + 20);
    d->sample_period_us = rd_u32(buf + 28);
    d->sample_count     = rd_u16(buf + 32);
    d->payload_len      = payload_len;
    d->payload          = payload_len > 0 ? buf + PKT_DATA_HDR_LEN : NULL;

    return data_fields_valid(d) ? PKT_OK : PKT_ERR_FIELD;
}

static int decode_heartbeat(const uint8_t *buf, size_t len, pkt_heartbeat_t *h)
{
    if (len < PKT_HEARTBEAT_LEN)
        return PKT_ERR_SHORT;
    if (len != PKT_HEARTBEAT_LEN)
        return PKT_ERR_LEN;
    if (!crc_ok(buf, len))
        return PKT_ERR_CRC;

    h->station_id    = rd_u32(buf + 4);
    h->next_seq      = rd_u64(buf + 8);
    h->oldest_seq    = rd_u64(buf + 16);
    h->uptime_s      = rd_u32(buf + 24);
    h->clock_quality = (uint8_t)(buf[28] & FLAG_CLOCK_MASK);

    if (h->clock_quality > PKT_CLOCK_LOCKED)
        return PKT_ERR_FIELD;
    if (h->oldest_seq > h->next_seq)
        return PKT_ERR_FIELD;
    return PKT_OK;
}

static int decode_gaps(const uint8_t *buf, size_t len, pkt_gaps_t *g)
{
    if (len < PKT_GAPS_HDR_LEN + PKT_CRC_LEN)
        return PKT_ERR_SHORT;

    const uint8_t count = buf[8];
    if (count > PKT_GAPS_MAX_RANGES)
        return PKT_ERR_LEN;
    if (len != PKT_GAPS_HDR_LEN + (size_t)count * PKT_GAPS_RANGE_LEN + PKT_CRC_LEN)
        return PKT_ERR_LEN;
    if (!crc_ok(buf, len))
        return PKT_ERR_CRC;

    g->station_id = rd_u32(buf + 4);
    g->count      = count;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *r = buf + PKT_GAPS_HDR_LEN + i * PKT_GAPS_RANGE_LEN;
        g->ranges[i].from = rd_u64(r);
        g->ranges[i].to   = rd_u64(r + 8);
        if (g->ranges[i].from > g->ranges[i].to)
            return PKT_ERR_FIELD;
    }
    return PKT_OK;
}

int pkt_decode(const uint8_t *buf, size_t len, pkt_t *out)
{
    if (buf == NULL || out == NULL)
        return PKT_ERR_ARG;
    if (len < 4)
        return PKT_ERR_SHORT;
    if (buf[0] != MAGIC0 || buf[1] != MAGIC1)
        return PKT_ERR_MAGIC;
    if (buf[2] != PKT_VERSION)
        return PKT_ERR_VERSION;

    memset(out, 0, sizeof *out);
    switch (buf[3]) {
    case PKT_DATA:
        out->type = PKT_DATA;
        return decode_data(buf, len, &out->u.data);
    case PKT_HEARTBEAT:
        out->type = PKT_HEARTBEAT;
        return decode_heartbeat(buf, len, &out->u.heartbeat);
    case PKT_GAPS:
        out->type = PKT_GAPS;
        return decode_gaps(buf, len, &out->u.gaps);
    default:
        return PKT_ERR_TYPE;
    }
}

const char *pkt_strerror(int rc)
{
    switch (rc) {
    case PKT_OK:          return "ok";
    case PKT_ERR_ARG:     return "bad argument";
    case PKT_ERR_SHORT:   return "datagram too short";
    case PKT_ERR_MAGIC:   return "bad magic";
    case PKT_ERR_VERSION: return "unsupported version";
    case PKT_ERR_TYPE:    return "unknown message type";
    case PKT_ERR_LEN:     return "length mismatch";
    case PKT_ERR_CRC:     return "crc mismatch";
    case PKT_ERR_FIELD:   return "field out of range";
    case PKT_ERR_SIZE:    return "buffer too small";
    default:              return "unknown error";
    }
}
