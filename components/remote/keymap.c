#include <stddef.h>
#include "remote.h"

static const remote_key_t s_keys[] = {
    { "edifier-s2000mkii", IR_NEC, 0xe710, 0x54, "previous" },
    { "edifier-s2000mkii", IR_NEC, 0xe710, 0x50, "play-pause" },
    { "edifier-s2000mkii", IR_NEC, 0xe710, 0x13, "next" },
};

const remote_key_t *remote_lookup(const ir_frame_t *f)
{
    ir_proto_t proto = f->proto == IR_REPEAT ? IR_NEC : f->proto;
    if (!f->valid) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(s_keys) / sizeof(s_keys[0]); i++) {
        if (s_keys[i].proto == proto && s_keys[i].addr == f->addr && s_keys[i].cmd == f->cmd) {
            return &s_keys[i];
        }
    }
    return NULL;
}

const remote_key_t *remote_keys(size_t *count)
{
    *count = sizeof(s_keys) / sizeof(s_keys[0]);
    return s_keys;
}
