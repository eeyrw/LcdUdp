/*
 * Global application state.
 */

#pragma once

#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_STATE_WIFI_DISCONNECTED,
    APP_STATE_WIFI_SMARTCONFIG,
    APP_STATE_WIFI_CONNECTING,
    APP_STATE_WIFI_CONNECTED,
    APP_STATE_RUNNING,
} app_state_t;

static _Atomic int s_app_state = APP_STATE_WIFI_DISCONNECTED;

static inline app_state_t app_get_state(void)
{
    return (app_state_t)atomic_load(&s_app_state);
}

static inline void app_set_state(app_state_t state)
{
    atomic_store(&s_app_state, (int)state);
}

#ifdef __cplusplus
}
#endif
