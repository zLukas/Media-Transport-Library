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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "app_base.h"
#include "log.h"
#include "player.h"

#ifndef _RX_APP_VIDEO_HEAD_H_
#define _RX_APP_VIDEO_HEAD_H_

int st_app_rx_video_sessions_init(struct st_app_context* ctx);

int st_app_rx_video_sessions_uinit(struct st_app_context* ctx);

int st_app_rx_video_sessions_stat(struct st_app_context* ctx);

int st_app_rx_video_sessions_result(struct st_app_context* ctx);

#endif