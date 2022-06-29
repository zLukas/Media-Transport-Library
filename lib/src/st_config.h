/*
 * Copyright (C) 2022 Intel Corporation.
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

#ifndef _ST_LIB_CONFIG_HEAD_H_
#define _ST_LIB_CONFIG_HEAD_H_

#include "st_main.h"

int st_config_init(struct st_main_impl* impl);
int st_config_uinit(struct st_main_impl* impl);

#endif
