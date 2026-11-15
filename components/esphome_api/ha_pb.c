// The protobuf subset the ESPHome API needs: varint, fixed32/float, length-delimited.
#include <string.h>

#include "ha_internal.h"

static void put(pb_w_t *w, const void *p, size_t n)
{
    if (w->ovf || w->len + n > w->cap) {
        w->ovf = true;
        return;
    }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}

static void raw_varint(pb_w_t *w, uint64_t v)
{
    uint8_t b[10];
    size_t n = 0;
    do {
        b[n] = v & 0x7F;
        v >>= 7;
        if (v) b[n] |= 0x80;
        n++;
    } while (v);
    put(w, b, n);
}

static void tag(pb_w_t *w, uint32_t field, uint8_t wire)
{
    raw_varint(w, ((uint64_t)field << 3) | wire);
}

void pb_varint(pb_w_t *w, uint32_t field, uint64_t v)
{
    if (!v) return;
    tag(w, field, 0);
    raw_varint(w, v);
}

void pb_bool(pb_w_t *w, uint32_t field, bool v)
{
    pb_varint(w, field, v ? 1 : 0);
}

void pb_fixed32(pb_w_t *w, uint32_t field, uint32_t v)
{
    uint8_t b[4] = {v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24};
    tag(w, field, 5);
    put(w, b, 4);
}

void pb_float(pb_w_t *w, uint32_t field, float v)
{
    uint32_t u;
    memcpy(&u, &v, 4);
    pb_fixed32(w, field, u);
}

void pb_bytes(pb_w_t *w, uint32_t field, const void *p, size_t n)
{
    tag(w, field, 2);
    raw_varint(w, n);
    put(w, p, n);
}

void pb_str(pb_w_t *w, uint32_t field, const char *s)
{
    if (s && *s) pb_bytes(w, field, s, strlen(s));
}

static bool read_varint(pb_r_t *r, uint64_t *v)
{
    *v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) return false;
        uint8_t b = *r->p++;
        *v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

bool pb_next(pb_r_t *r, pb_field_t *f)
{
    if (!r->p || r->p >= r->end) return false;
    uint64_t t;
    if (!read_varint(r, &t) || (t >> 3) == 0 || (t >> 3) > 0x1FFFFFFF) goto bad;
    f->field = (uint32_t)(t >> 3);
    f->wire = t & 7;
    f->v = 0;
    f->data = NULL;
    f->len = 0;
    switch (f->wire) {
    case 0:
        if (!read_varint(r, &f->v)) goto bad;
        return true;
    case 1:
        if (r->end - r->p < 8) goto bad;
        for (int i = 0; i < 8; i++) f->v |= (uint64_t)r->p[i] << (8 * i);
        r->p += 8;
        return true;
    case 2: {
        uint64_t n;
        if (!read_varint(r, &n) || n > (uint64_t)(r->end - r->p)) goto bad;
        f->data = r->p;
        f->len = (size_t)n;
        r->p += n;
        return true;
    }
    case 5:
        if (r->end - r->p < 4) goto bad;
        f->v = (uint32_t)r->p[0] | (uint32_t)r->p[1] << 8 | (uint32_t)r->p[2] << 16 | (uint32_t)r->p[3] << 24;
        r->p += 4;
        return true;
    default:
        goto bad;
    }
bad:
    r->p = NULL;
    return false;
}
