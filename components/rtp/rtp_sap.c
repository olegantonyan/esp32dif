#include <stdlib.h>
#include <string.h>
#include "rtp_priv.h"

#define SAP_VERSION 1
#define SAP_FLAG_IPV6 0x10
#define SAP_FLAG_DELETE 0x04
#define SAP_FLAG_ENCRYPTED 0x02
#define SAP_FLAG_COMPRESSED 0x01
#define SAP_HEADER_BYTES 4
#define SAP_MAX_SDP 1500

static const char SDP_TYPE[] = "application/sdp";

static void parse_line(const char *line, rtp_session_t *s, int *rtpmap_pt)
{
    if (!strncmp(line, "s=", 2)) {
        strlcpy(s->name, line + 2, sizeof(s->name));
    } else if (!strncmp(line, "m=audio ", 8)) {
        char *end;
        s->port = strtoul(line + 8, &end, 10);
        const char *pt = strrchr(end, ' ');
        s->pt = pt ? strtoul(pt + 1, NULL, 10) : 0;
    } else if (!strncmp(line, "a=rtpmap:", 9)) {
        char *end;
        *rtpmap_pt = strtol(line + 9, &end, 10);
        const char *enc = end + strspn(end, " ");
        size_t n = strcspn(enc, "/");
        if (n < sizeof(s->encoding) && enc[n] == '/') {
            memcpy(s->encoding, enc, n);
            s->encoding[n] = 0;
            s->rate = strtoul(enc + n + 1, &end, 10);
            s->channels = *end == '/' ? strtoul(end + 1, NULL, 10) : 1;
        }
    } else if (!strncmp(line, "a=ssrc:", 7)) {
        s->ssrc = strtoul(line + 7, NULL, 10);
    }
}

bool sap_parse(const uint8_t *p, int len, uint32_t origin, rtp_session_t *out, bool *deleted)
{
    if (len < SAP_HEADER_BYTES || p[0] >> 5 != SAP_VERSION || (p[0] & (SAP_FLAG_ENCRYPTED | SAP_FLAG_COMPRESSED))) {
        return false;
    }
    int pos = SAP_HEADER_BYTES + ((p[0] & SAP_FLAG_IPV6) ? 16 : 4) + 4 * p[1];
    if (pos >= len) {
        return false;
    }
    if (len - pos >= (int)sizeof(SDP_TYPE) && !memcmp(p + pos, SDP_TYPE, sizeof(SDP_TYPE))) {
        pos += sizeof(SDP_TYPE);
    }
    static char sdp[SAP_MAX_SDP + 1];
    int n = len - pos < SAP_MAX_SDP ? len - pos : SAP_MAX_SDP;
    memcpy(sdp, p + pos, n);
    sdp[n] = 0;
    if (strncmp(sdp, "v=0", 3)) {
        return false;
    }
    rtp_session_t s = { .origin_ip = origin };
    int rtpmap_pt = -1;
    for (char *line = strtok(sdp, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        parse_line(line, &s, &rtpmap_pt);
    }
    if (!s.port || rtpmap_pt != s.pt || !s.rate) {
        return false;
    }
    s.valid = true;
    *deleted = p[0] & SAP_FLAG_DELETE;
    *out = s;
    return true;
}
