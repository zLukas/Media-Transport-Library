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

#include "st22_pipeline_rx.h"

#include "../st_log.h"

static const char* st22p_rx_frame_stat_name[ST22P_RX_FRAME_STATUS_MAX] = {
    "free", "ready", "in_decoding", "decoded", "in_user",
};

static const char* rx_st22p_stat_name(enum st22p_rx_frame_status stat) {
  return st22p_rx_frame_stat_name[stat];
}

static uint16_t rx_st22p_next_idx(struct st22p_rx_ctx* ctx, uint16_t idx) {
  /* point to next */
  uint16_t next_idx = idx;
  next_idx++;
  if (next_idx >= ctx->framebuff_cnt) next_idx = 0;
  return next_idx;
}

static struct st22p_rx_frame* rx_st22p_next_available(
    struct st22p_rx_ctx* ctx, uint16_t idx_start, enum st22p_rx_frame_status desired) {
  uint16_t idx = idx_start;
  struct st22p_rx_frame* framebuff;

  /* check ready frame from idx_start */
  while (1) {
    framebuff = &ctx->framebuffs[idx];
    if (desired == framebuff->stat) {
      /* find one desired */
      return framebuff;
    }
    idx = rx_st22p_next_idx(ctx, idx);
    if (idx == idx_start) {
      /* loop all frames end */
      break;
    }
  }

  /* no any desired frame */
  return NULL;
}

static int rx_st22p_frame_ready(void* priv, void* frame, struct st22_frame_meta* meta) {
  struct st22p_rx_ctx* ctx = priv;
  struct st22p_rx_frame* framebuff;

  if (!ctx->ready) return -EBUSY; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st22p_next_available(ctx, ctx->framebuff_producer_idx, ST22P_RX_FRAME_FREE);
  /* not any free frame */
  if (!framebuff) {
    rte_atomic32_inc(&ctx->stat_busy);
    st_pthread_mutex_unlock(&ctx->lock);
    return -EBUSY;
  }

  framebuff->src.addr = frame;
  framebuff->src.data_size = meta->frame_total_size;
  framebuff->src.tfmt = meta->tfmt;
  framebuff->src.timestamp = meta->timestamp;
  framebuff->dst.tfmt = meta->tfmt;
  /* set dst timestamp to same as src? */
  framebuff->dst.timestamp = meta->timestamp;
  framebuff->stat = ST22P_RX_FRAME_READY;
  /* point to next */
  ctx->framebuff_producer_idx = rx_st22p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, ctx->idx, framebuff->idx);
  st22_decode_notify_frame_ready(ctx->decode_impl);

  return 0;
}

static struct st22_decode_frame_meta* rx_st22p_decode_get_frame(void* priv) {
  struct st22p_rx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st22p_rx_frame* framebuff;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st22p_next_available(ctx, ctx->framebuff_decode_idx, ST22P_RX_FRAME_READY);
  /* not any ready frame */
  if (!framebuff) {
    st_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST22P_RX_FRAME_IN_DECODING;
  /* point to next */
  ctx->framebuff_decode_idx = rx_st22p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->decode_frame;
}

static int rx_st22p_decode_put_frame(void* priv, struct st22_decode_frame_meta* frame,
                                     int result) {
  struct st22p_rx_ctx* ctx = priv;
  int idx = ctx->idx;
  struct st22p_rx_frame* framebuff = frame->priv;
  uint16_t decode_idx = framebuff->idx;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST22P_RX_FRAME_IN_DECODING != framebuff->stat) {
    err("%s(%d), frame %u not in decoding %d\n", __func__, idx, decode_idx,
        framebuff->stat);
    return -EIO;
  }

  dbg("%s(%d), frame %u result %d\n", __func__, idx, decode_idx, result);
  if (result < 0) {
    /* free the frame */
    st22_rx_put_framebuff(ctx->transport, framebuff->src.addr);
    framebuff->stat = ST22P_RX_FRAME_FREE;
    rte_atomic32_inc(&ctx->stat_decode_fail);
  } else {
    framebuff->stat = ST22P_RX_FRAME_DECODED;
    if (ctx->ops.notify_frame_available) { /* notify app */
      ctx->ops.notify_frame_available(ctx->ops.priv);
    }
  }

  return 0;
}

static int rx_st22p_decode_dump(void* priv) {
  struct st22p_rx_ctx* ctx = priv;
  struct st22p_rx_frame* framebuff = ctx->framebuffs;

  if (!ctx->ready) return -EBUSY; /* not ready */

  uint16_t producer_idx = ctx->framebuff_producer_idx;
  uint16_t decode_idx = ctx->framebuff_decode_idx;
  uint16_t consumer_idx = ctx->framebuff_consumer_idx;
  info("RX_ST22P(%s), p(%d:%s) d(%d:%s) c(%d:%s)\n", ctx->ops_name, producer_idx,
       rx_st22p_stat_name(framebuff[producer_idx].stat), decode_idx,
       rx_st22p_stat_name(framebuff[decode_idx].stat), consumer_idx,
       rx_st22p_stat_name(framebuff[consumer_idx].stat));

  int decode_fail = rte_atomic32_read(&ctx->stat_decode_fail);
  rte_atomic32_set(&ctx->stat_decode_fail, 0);
  if (decode_fail) {
    info("RX_ST22P(%s), decode fail %d\n", ctx->ops_name, decode_fail);
  }

  int busy = rte_atomic32_read(&ctx->stat_busy);
  rte_atomic32_set(&ctx->stat_busy, 0);
  if (busy) {
    info("RX_ST22P(%s), busy drop frame %d\n", ctx->ops_name, busy);
  }

  return 0;
}

static int rx_st22p_create_transport(st_handle st, struct st22p_rx_ctx* ctx,
                                     struct st22p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st22_rx_ops ops_rx;
  st22_rx_handle transport;

  memset(&ops_rx, 0, sizeof(ops_rx));
  ops_rx.name = ops->name;
  ops_rx.priv = ctx;
  ops_rx.num_port = RTE_MIN(ops->port.num_port, ST_PORT_MAX);
  for (int i = 0; i < ops_rx.num_port; i++) {
    memcpy(ops_rx.sip_addr[i], ops->port.sip_addr[i], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[i], ops->port.port[i], ST_PORT_MAX_LEN);
    ops_rx.udp_port[i] = ops->port.udp_port[i] + i;
  }
  ops_rx.pacing = ST21_PACING_NARROW;
  ops_rx.width = ops->width;
  ops_rx.height = ops->height;
  ops_rx.fps = ops->fps;
  ops_rx.payload_type = ops->port.payload_type;
  ops_rx.type = ST22_TYPE_FRAME_LEVEL;
  ops_rx.pack_type = ops->pack_type;
  ops_rx.framebuff_cnt = ops->framebuff_cnt;
  ops_rx.framebuff_max_size = ctx->max_codestream_size;
  ops_rx.notify_frame_ready = rx_st22p_frame_ready;

  transport = st22_rx_create(st, &ops_rx);
  if (!transport) {
    err("%s(%d), transport create fail\n", __func__, idx);
    return -EIO;
  }
  ctx->transport = transport;

  struct st22p_rx_frame* frames = ctx->framebuffs;
  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].src.fmt = ctx->decode_impl->req.req.input_fmt;
    frames[i].src.buffer_size = ops_rx.framebuff_max_size;
    frames[i].src.data_size = ops_rx.framebuff_max_size;
    frames[i].src.width = ops->width;
    frames[i].src.height = ops->height;
    frames[i].src.idx = i;
    frames[i].src.priv = &frames[i];

    frames[i].decode_frame.src = &frames[i].src;
    frames[i].decode_frame.dst = &frames[i].dst;
    frames[i].decode_frame.priv = &frames[i];
  }

  return 0;
}

static int rx_st22p_uinit_dst_fbs(struct st22p_rx_ctx* ctx) {
  if (ctx->framebuffs) {
    for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
      if (ctx->framebuffs[i].dst.addr) {
        st_rte_free(ctx->framebuffs[i].dst.addr);
        ctx->framebuffs[i].dst.addr = NULL;
      }
    }
    st_rte_free(ctx->framebuffs);
    ctx->framebuffs = NULL;
  }

  return 0;
}

static int rx_st22p_init_dst_fbs(struct st_main_impl* impl, struct st22p_rx_ctx* ctx,
                                 struct st22p_rx_ops* ops) {
  int idx = ctx->idx;
  int soc_id = st_socket_id(impl, ST_PORT_P);
  struct st22p_rx_frame* frames;
  void* dst;
  size_t dst_size = ctx->dst_size;

  ctx->framebuff_cnt = ops->framebuff_cnt;
  frames = st_rte_zmalloc_socket(sizeof(*frames) * ctx->framebuff_cnt, soc_id);
  if (!frames) {
    err("%s(%d), frames malloc fail\n", __func__, idx);
    return -ENOMEM;
  }
  ctx->framebuffs = frames;

  for (uint16_t i = 0; i < ctx->framebuff_cnt; i++) {
    frames[i].stat = ST22P_RX_FRAME_FREE;
    frames[i].idx = i;
    dst = st_rte_zmalloc_socket(dst_size, soc_id);
    if (!dst) {
      err("%s(%d), src frame malloc fail at %u\n", __func__, idx, i);
      rx_st22p_uinit_dst_fbs(ctx);
      return -ENOMEM;
    }
    frames[i].dst.addr = dst;
    frames[i].dst.fmt = ops->output_fmt;
    frames[i].dst.buffer_size = dst_size;
    frames[i].dst.data_size = dst_size;
    frames[i].dst.width = ops->width;
    frames[i].dst.height = ops->height;
    frames[i].dst.idx = i;
    frames[i].dst.priv = &frames[i];
  }

  info("%s(%d), size %ld fmt %d with %u frames\n", __func__, idx, dst_size,
       ops->output_fmt, ctx->framebuff_cnt);
  return 0;
}

static int rx_st22p_get_decoder(struct st_main_impl* impl, struct st22p_rx_ctx* ctx,
                                struct st22p_rx_ops* ops) {
  int idx = ctx->idx;
  struct st22_get_decoder_request req;

  memset(&req, 0, sizeof(req));
  req.codec = ops->codec;
  req.device = ops->device;
  req.req.width = ops->width;
  req.req.height = ops->height;
  req.req.fps = ops->fps;
  req.req.output_fmt = ops->output_fmt;
  if (req.codec == ST22_CODEC_JPEGXS) {
    req.req.input_fmt = ST_FRAME_FMT_JPEGXS_CODESTREAM;
  } else {
    err("%s(%d), unknow codec %d\n", __func__, idx, req.codec);
    return -EINVAL;
  }
  req.req.framebuff_cnt = ops->framebuff_cnt;
  req.req.codec_thread_cnt = ops->codec_thread_cnt;
  req.priv = ctx;
  req.get_frame = rx_st22p_decode_get_frame;
  req.put_frame = rx_st22p_decode_put_frame;
  req.dump = rx_st22p_decode_dump;

  struct st22_decode_session_impl* decode_impl = st22_get_decoder(impl, &req);
  if (!decode_impl) {
    err("%s(%d), get decoder fail\n", __func__, idx);
    return -EINVAL;
  }
  ctx->decode_impl = decode_impl;

  return 0;
}

struct st_frame_meta* st22p_rx_get_frame(st22p_rx_handle handle) {
  struct st22p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st22p_rx_frame* framebuff;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return NULL;
  }

  if (!ctx->ready) return NULL; /* not ready */

  st_pthread_mutex_lock(&ctx->lock);
  framebuff =
      rx_st22p_next_available(ctx, ctx->framebuff_consumer_idx, ST22P_RX_FRAME_DECODED);
  /* not any decoded frame */
  if (!framebuff) {
    st_pthread_mutex_unlock(&ctx->lock);
    return NULL;
  }

  framebuff->stat = ST22P_RX_FRAME_IN_USER;
  /* point to next */
  ctx->framebuff_consumer_idx = rx_st22p_next_idx(ctx, framebuff->idx);
  st_pthread_mutex_unlock(&ctx->lock);

  dbg("%s(%d), frame %u succ\n", __func__, idx, framebuff->idx);
  return &framebuff->dst;
}

int st22p_rx_put_frame(st22p_rx_handle handle, struct st_frame_meta* frame) {
  struct st22p_rx_ctx* ctx = handle;
  int idx = ctx->idx;
  struct st22p_rx_frame* framebuff = frame->priv;
  uint16_t consumer_idx = framebuff->idx;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, idx, ctx->type);
    return -EIO;
  }

  if (ST22P_RX_FRAME_IN_USER != framebuff->stat) {
    err("%s(%d), frame %u not in free %d\n", __func__, idx, consumer_idx,
        framebuff->stat);
    return -EIO;
  }

  /* free the frame */
  st22_rx_put_framebuff(ctx->transport, framebuff->src.addr);
  framebuff->stat = ST22P_RX_FRAME_FREE;
  dbg("%s(%d), frame %u succ\n", __func__, idx, consumer_idx);

  return 0;
}

st22p_rx_handle st22p_rx_create(st_handle st, struct st22p_rx_ops* ops) {
  struct st_main_impl* impl = st;
  struct st22p_rx_ctx* ctx;
  int ret;
  int idx = 0; /* todo */
  size_t dst_size;

  if (impl->type != ST_SESSION_TYPE_MAIN) {
    err("%s, invalid type %d\n", __func__, impl->type);
    return NULL;
  }

  if (!ops->notify_frame_available) {
    err("%s, pls set notify_frame_available\n", __func__);
    return NULL;
  }

  dst_size = st_frame_size(ops->output_fmt, ops->width, ops->height);
  if (!dst_size) {
    err("%s(%d), get dst size fail\n", __func__, idx);
    return NULL;
  }

  ctx = st_rte_zmalloc_socket(sizeof(*ctx), st_socket_id(impl, ST_PORT_P));
  if (!ctx) {
    err("%s, ctx malloc fail\n", __func__);
    return NULL;
  }

  ctx->idx = idx;
  ctx->ready = false;
  ctx->impl = impl;
  ctx->type = ST22_SESSION_TYPE_PIPELINE_RX;
  ctx->dst_size = dst_size;
  /* use the possible max size */
  ctx->max_codestream_size = ops->max_codestream_size;
  if (!ctx->max_codestream_size) ctx->max_codestream_size = dst_size;
  rte_atomic32_set(&ctx->stat_decode_fail, 0);
  rte_atomic32_set(&ctx->stat_busy, 0);
  st_pthread_mutex_init(&ctx->lock, NULL);

  /* copy ops */
  strncpy(ctx->ops_name, ops->name, ST_MAX_NAME_LEN - 1);
  ctx->ops = *ops;

  /* get one suitable jpegxs decode device */
  ret = rx_st22p_get_decoder(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), get decoder fail %d\n", __func__, idx, ret);
    st22p_rx_free(ctx);
    return NULL;
  }

  /* init fbs */
  ret = rx_st22p_init_dst_fbs(impl, ctx, ops);
  if (ret < 0) {
    err("%s(%d), init fbs fail %d\n", __func__, idx, ret);
    st22p_rx_free(ctx);
    return NULL;
  }

  /* crete transport handle */
  ret = rx_st22p_create_transport(st, ctx, ops);
  if (ret < 0) {
    err("%s(%d), create transport fail\n", __func__, idx);
    st22p_rx_free(ctx);
    return NULL;
  }

  /* all ready now */
  ctx->ready = true;

  if (ctx->ops.notify_frame_available) { /* notify app */
    ctx->ops.notify_frame_available(ctx->ops.priv);
  }

  return ctx;
}

int st22p_rx_free(st22p_rx_handle handle) {
  struct st22p_rx_ctx* ctx = handle;
  struct st_main_impl* impl = ctx->impl;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, ctx->idx, ctx->type);
    return -EIO;
  }

  if (ctx->decode_impl) {
    st22_put_decoder(impl, ctx->decode_impl);
    ctx->decode_impl = NULL;
  }

  if (ctx->transport) {
    st22_rx_free(ctx->transport);
    ctx->transport = NULL;
  }
  rx_st22p_uinit_dst_fbs(ctx);

  st_pthread_mutex_destroy(&ctx->lock);
  st_rte_free(ctx);

  return 0;
}

void* st22p_rx_get_fb_addr(st22p_rx_handle handle, uint16_t idx) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return NULL;
  }

  if (idx < 0 || idx >= ctx->framebuff_cnt) {
    err("%s, invalid idx %d, should be in range [0, %d]\n", __func__, cidx,
        ctx->framebuff_cnt);
    return NULL;
  }

  return ctx->framebuffs[idx].dst.addr;
}

size_t st22p_rx_frame_size(st22p_rx_handle handle) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return 0;
  }

  return ctx->dst_size;
}

int st22p_rx_pcapng_dump(st22p_rx_handle handle, uint32_t max_dump_packets, bool sync,
                         struct st_pcap_dump_meta* meta) {
  struct st22p_rx_ctx* ctx = handle;
  int cidx = ctx->idx;

  if (ctx->type != ST22_SESSION_TYPE_PIPELINE_RX) {
    err("%s(%d), invalid type %d\n", __func__, cidx, ctx->type);
    return -EIO;
  }

  return st22_rx_pcapng_dump(ctx->transport, max_dump_packets, sync, meta);
}
