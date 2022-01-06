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

#ifndef _ST_LIB_FMT_HEAD_H_
#define _ST_LIB_FMT_HEAD_H_

#include <st_dpdk_api.h>

struct st_fps_timing {
  enum st_fps fps;
  int sampling_clock_rate; /* 90k of sampling clock rate */
  int mul;                 /* 60000 for ST_FPS_P59_94 */
  int den;                 /* 1001 for ST_FPS_P59_94 */
  double frame_rate;       /* fps number */
};

int st_get_fps_timing(enum st_fps fps, struct st_fps_timing* fps_tm);

/* bit per sec */
int st20_get_bandwidth_bps(int width, int height, enum st20_fmt fmt, enum st_fps fps,
                           uint64_t* bps);

#endif
