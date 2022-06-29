/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

#ifndef _ST_LIB_DEV_HEAD_H_
#define _ST_LIB_DEV_HEAD_H_

#include "st_main.h"

#define ST_DEV_RX_DESC (4096 / 2)
#define ST_DEV_TX_DESC (4096 / 8)

#define ST_DEV_STAT_INTERVAL_S (10) /* 10s */
#define ST_DEV_STAT_INTERVAL_US(s) (s * US_PER_S)
#define ST_DEV_STAT_M_UNIT (1000 * 1000)

#define ST_EAL_MAX_ARGS (32)

int st_dev_get_socket(const char* port);

int st_dev_init(struct st_init_params* p);
int st_dev_uinit(struct st_init_params* p);

int st_dev_create(struct st_main_impl* impl);
int st_dev_free(struct st_main_impl* impl);

int st_dev_start(struct st_main_impl* impl);
int st_dev_stop(struct st_main_impl* impl);

/* ip from 224.x.x.x to 239.x.x.x */
static inline uint64_t st_is_multicast_ip(uint8_t ip[ST_IP_ADDR_LEN]) {
  if (ip[0] >= 224 && ip[0] <= 239)
    return true;
  else
    return false;
}

int st_dev_dst_ip_mac(struct st_main_impl* impl, uint8_t dip[ST_IP_ADDR_LEN],
                      struct rte_ether_addr* ea, enum st_port port);

int st_dev_request_tx_queue(struct st_main_impl* impl, enum st_port port,
                            uint16_t* queue_id, uint64_t bytes_per_sec);
int st_dev_request_rx_queue(struct st_main_impl* impl, enum st_port port,
                            uint16_t* queue_id, struct st_rx_flow* flow);
int st_dev_free_tx_queue(struct st_main_impl* impl, enum st_port port, uint16_t queue_id);
int st_dev_free_rx_queue(struct st_main_impl* impl, enum st_port port, uint16_t queue_id);
int st_dev_flush_tx_queue(struct st_main_impl* impl, enum st_port port,
                          uint16_t queue_id);

int st_dev_if_init(struct st_main_impl* impl);
int st_dev_if_uinit(struct st_main_impl* impl);

int st_dev_put_lcore(struct st_main_impl* impl, unsigned int lcore);
int st_dev_get_lcore(struct st_main_impl* impl, unsigned int* lcore);
bool st_dev_lcore_valid(struct st_main_impl* impl, unsigned int lcore);

#endif
