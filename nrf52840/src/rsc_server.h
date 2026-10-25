#pragma once

#include "data_bridge.h"

typedef void (*rsc_conn_cb_t)(bool connected);

void rsc_server_init(rsc_conn_cb_t conn_cb);   // after Bluefruit.begin()
void rsc_server_start_advertising(void);
bool rsc_server_notify(const rsc_data_t *data);
bool rsc_server_is_connected(void);
