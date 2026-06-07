#ifndef TV_BRIDGE_WORKER_H
#define TV_BRIDGE_WORKER_H

#include "ctm_settings.h"   /* tv_bridge_kind_t, audio mode, *_settings_t */

typedef struct tv_bridge_worker tv_bridge_worker_t;

typedef struct {
    char host[64];
    int port;
    char path[64];
    char bt_address[64];
    unsigned int vid;
    unsigned int pid;
    tv_bridge_worker_settings_t settings;
} tv_bridge_worker_config_t;

int tv_bridge_worker_start(tv_bridge_worker_t **out, const tv_bridge_worker_config_t *cfg);
void tv_bridge_worker_update_settings(tv_bridge_worker_t *worker,
                                      const tv_bridge_worker_settings_t *settings);
void tv_bridge_worker_stop(tv_bridge_worker_t *worker);

#endif
