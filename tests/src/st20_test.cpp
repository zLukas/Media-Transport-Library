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

#include <thread>

#include "log.h"
#include "tests.h"

static int tx_video_build_rtp_packet(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
                                     uint16_t* pkt_len) {
  uint16_t data_len = s->pkt_data_len;
  int pkts_in_line = s->pkts_in_line;
  int row_number = s->pkt_idx / pkts_in_line;
  int pixels_in_pkt = s->pkt_data_len / s->st20_pg.size * s->st20_pg.coverage;
  int row_offset = pixels_in_pkt * (s->pkt_idx % pkts_in_line);

  /* update hdr */
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = 2;
  rtp->base.marker = 0;
  rtp->base.payload_type = 96;
  rtp->row_number = htons(row_number);
  rtp->row_offset = htons(row_offset);
  rtp->base.tmstamp = htonl(s->rtp_tmstamp);
  rtp->base.seq_number = htons(s->seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->seq_id >> 16));
  s->seq_id++;
  int temp = (s->width - row_offset) / s->st20_pg.coverage * s->st20_pg.size;
  data_len = s->pkt_data_len > temp ? temp : s->pkt_data_len;
  rtp->row_length = htons(data_len);
  *pkt_len = data_len + sizeof(*rtp);
  if (s->check_md5) {
    uint8_t* payload = (uint8_t*)rtp + sizeof(struct st20_rfc4175_rtp_hdr);
    st_memcpy(
        payload,
        s->frame_buf[s->fb_idx % TEST_MD5_HIST_NUM] +
            (row_number * s->width + row_offset) / s->st20_pg.coverage * s->st20_pg.size,
        data_len);
  }

  s->pkt_idx++;
  if (s->pkt_idx >= s->total_pkts_in_frame) {
    /* end of current frame */
    rtp->base.marker = 1;

    s->pkt_idx = 0;
    s->fb_idx++;
    s->rtp_tmstamp++;
    s->fb_send++;
  }

  return 0;
}

static void tx_feed_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st20_tx_get_mbuf(ctx->handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    tx_video_build_rtp_packet(ctx, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);

    st20_tx_put_mbuf(ctx->handle, mbuf, mbuf_len);
  }
}

static int tx_rtp_done(void* args) {
  auto ctx = (tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static int rx_rtp_ready(void* args) {
  auto ctx = (tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx);
  ctx->cv.notify_all();
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  return 0;
}

static void rx_handle_rtp(tests_context* s, struct st20_rfc4175_rtp_hdr* hdr,
                          bool newframe) {
  int idx = s->idx;
  struct st20_rfc4175_extra_rtp_hdr* e_hdr = NULL;
  uint16_t row_number; /* 0 to 1079 for 1080p */
  uint16_t row_offset; /* [0, 480, 960, 1440] for 1080p */
  uint16_t row_length; /* 1200 for 1080p */
  uint8_t* frame;
  uint8_t* payload;

  if (newframe) {
    if (s->frame_buf[0]) {
      std::unique_lock<std::mutex> lck(s->mtx);
      s->buf_q.push(s->frame_buf[0]);
      s->cv.notify_all();
    }
    s->frame_buf[0] = (uint8_t*)st_test_zmalloc(s->frame_size);
    ASSERT_TRUE(s->frame_buf[0] != NULL);
  }

  frame = s->frame_buf[0];
  payload = (uint8_t*)hdr + sizeof(*hdr);
  row_number = ntohs(hdr->row_number);
  row_offset = ntohs(hdr->row_offset);
  row_length = ntohs(hdr->row_length);
  dbg("%s(%d), row: %d %d %d\n", __func__, idx, row_number, row_offset, row_length);
  if (row_offset & ST20_SRD_OFFSET_CONTINUATION) {
    /* additional Sample Row Data */
    row_offset &= ~ST20_SRD_OFFSET_CONTINUATION;
    e_hdr = (struct st20_rfc4175_extra_rtp_hdr*)payload;
    payload += sizeof(*e_hdr);
  }

  /* copy the payload to target frame */
  uint32_t offset =
      (row_number * s->width + row_offset) / s->st20_pg.coverage * s->st20_pg.size;
  if ((offset + row_length) > s->frame_size) {
    err("%s(%d: invalid offset %u frame size %ld\n", __func__, idx, offset,
        s->frame_size);
    return;
  }
  st_memcpy(frame + offset, payload, row_length);
  if (e_hdr) {
    uint16_t row2_number = ntohs(e_hdr->row_number);
    uint16_t row2_offset = ntohs(e_hdr->row_offset);
    uint16_t row2_length = ntohs(e_hdr->row_length);

    dbg("%s(%d), row: %d %d %d\n", __func__, idx, row2_number, row2_offset, row2_length);
    uint32_t offset2 =
        (row2_number * s->width + row2_offset) / s->st20_pg.coverage * s->st20_pg.size;
    if ((offset2 + row2_length) > s->frame_size) {
      err("%s(%d: invalid offset %u frame size %ld for extra hdr\n", __func__, idx,
          offset2, s->frame_size);
      return;
    }
    st_memcpy(frame + offset2, payload + row_length, row2_length);
  }

  return;
}

static void rx_get_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  struct st20_rfc4175_rtp_hdr* hdr;
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st20_rx_get_mbuf(ctx->handle, &usrptr, &mbuf_len);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st20_rx_get_mbuf(ctx->handle, &usrptr, &mbuf_len);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }
    hdr = (struct st20_rfc4175_rtp_hdr*)usrptr;
    int32_t tmstamp = ntohl(hdr->base.tmstamp);
    bool newframe = false;
    if (tmstamp != ctx->rtp_tmstamp) {
      /* new frame received */
      ctx->rtp_tmstamp = tmstamp;
      ctx->fb_rec++;
      newframe = true;
    }
    if (ctx->check_md5) {
      rx_handle_rtp(ctx, hdr, newframe);
    }
    st20_rx_put_mbuf(ctx->handle, mbuf);
  }
}

static int st20_rx_frame_ready(void* priv, void* frame, struct st20_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO;

  if (st20_is_frame_complete(meta->status)) {
    ctx->fb_rec++;
    if (!ctx->start_time) {
      ctx->rtp_delta = meta->timestamp - ctx->rtp_tmstamp;
      ctx->start_time = st_test_get_monotonic_time();
    }
  }
  if (meta->tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) ctx->rtp_tmstamp = meta->timestamp;
  st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
  return 0;
}

static void st20_tx_ops_init(tests_context* st20, struct st20_tx_ops* ops) {
  auto ctx = st20->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st20_test";
  ops->priv = st20;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 10000 + st20->idx;
  if (ops->num_port == 2) {
    memcpy(ops->dip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 10000 + st20->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->type = ST20_TYPE_FRAME_LEVEL;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->fmt = ST20_FMT_YUV_422_10BIT;

  ops->framebuff_cnt = st20->fb_cnt;
  ops->get_next_frame = tx_next_frame;
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
}

static void st20_rx_ops_init(tests_context* st20, struct st20_rx_ops* ops) {
  auto ctx = st20->ctx;

  memset(ops, 0, sizeof(*ops));
  ops->name = "st20_test";
  ops->priv = st20;
  ops->num_port = ctx->para.num_ports;
  memcpy(ops->sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  strncpy(ops->port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
  ops->udp_port[ST_PORT_P] = 10000 + st20->idx;
  if (ops->num_port == 2) {
    memcpy(ops->sip_addr[ST_PORT_R], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops->port[ST_PORT_R], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops->udp_port[ST_PORT_R] = 10000 + st20->idx;
  }
  ops->pacing = ST21_PACING_NARROW;
  ops->type = ST20_TYPE_FRAME_LEVEL;
  ops->width = 1920;
  ops->height = 1080;
  ops->fps = ST_FPS_P59_94;
  ops->fmt = ST20_FMT_YUV_422_10BIT;

  ops->framebuff_cnt = st20->fb_cnt;
  ops->notify_frame_ready = st20_rx_frame_ready;
  ops->notify_rtp_ready = rx_rtp_ready;
  ops->rtp_ring_size = 1024;
}

static void st20_tx_assert_cnt(int expect_s20_tx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st20_tx_sessions_cnt, expect_s20_tx_cnt);
}

static void st20_rx_assert_cnt(int expect_s20_rx_cnt) {
  auto ctx = st_test_ctx();
  auto handle = ctx->handle;
  struct st_stats stats;
  int ret;

  ret = st_get_stats(handle, &stats);
  EXPECT_GE(ret, 0);
  EXPECT_EQ(stats.st20_rx_sessions_cnt, expect_s20_rx_cnt);
}

TEST(St20_tx, create_free_single) { create_free_test(st20_tx, 0, 1, 1); }
TEST(St20_tx, create_free_multi) { create_free_test(st20_tx, 0, 1, 6); }
TEST(St20_tx, create_free_mix) { create_free_test(st20_tx, 2, 3, 4); }
TEST(St20_tx, create_free_max) { create_free_max(st20_tx, 100); }
TEST(St20_tx, create_expect_fail) { expect_fail_test(st20_tx); }
TEST(St20_tx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 1;
  expect_fail_test_fb_cnt(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st20_tx, fbcnt);
}
TEST(St20_tx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st20_tx, ST20_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st20_tx, ST20_TYPE_RTP_LEVEL, ring_size);
}
TEST(St20_tx, get_framebuffer) {
  uint16_t fbcnt = 3;
  test_get_framebuffer(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT;
  test_get_framebuffer(st20_tx, fbcnt);
}
TEST(St20_tx, get_framebuffer_expect_fail) {
  uint16_t fbcnt = 3;
  expect_fail_test_get_framebuffer(st20_tx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT;
  expect_fail_test_get_framebuffer(st20_tx, fbcnt);
}
TEST(St20_tx, rtp_pkt_size) {
  uint16_t rtp_pkt_size = 0;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, false);
  rtp_pkt_size = ST_PKT_MAX_RTP_BYTES;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, true);
  rtp_pkt_size = ST_PKT_MAX_RTP_BYTES + 1;
  expect_test_rtp_pkt_size(st20_tx, ST20_TYPE_RTP_LEVEL, rtp_pkt_size, false);
}

TEST(St20_rx, create_free_single) { create_free_test(st20_rx, 0, 1, 1); }
TEST(St20_rx, create_free_multi) { create_free_test(st20_rx, 0, 1, 6); }
TEST(St20_rx, create_free_mix) { create_free_test(st20_rx, 2, 3, 4); }
TEST(St20_rx, create_free_max) { create_free_max(st20_rx, 100); }
TEST(St20_rx, create_expect_fail) { expect_fail_test(st20_rx); }
TEST(St20_rx, create_expect_fail_fb_cnt) {
  uint16_t fbcnt = 0;
  expect_fail_test_fb_cnt(st20_rx, fbcnt);
  fbcnt = ST20_FB_MAX_COUNT + 1;
  expect_fail_test_fb_cnt(st20_rx, fbcnt);
}
TEST(St20_rx, create_expect_fail_ring_sz) {
  uint16_t ring_size = 0;
  expect_fail_test_rtp_ring(st20_rx, ST20_TYPE_RTP_LEVEL, ring_size);
  ring_size = 128 + 1;
  expect_fail_test_rtp_ring(st20_rx, ST20_TYPE_RTP_LEVEL, ring_size);
}

static void rtp_tx_specific_init(struct st20_tx_ops* ops, tests_context* test_ctx) {
  int ret;
  ret = st20_get_pgroup(ops->fmt, &test_ctx->st20_pg);
  ASSERT_TRUE(ret == 0);
  /* calculate pkts in line for rtp */
  size_t bytes_in_pkt = ST_PKT_MAX_RTP_BYTES - sizeof(struct st20_rfc4175_rtp_hdr);
  /* 4800 if 1080p yuv422 */
  size_t bytes_in_line = ops->width * test_ctx->st20_pg.size / test_ctx->st20_pg.coverage;

  int pkts_in_line = (bytes_in_line / bytes_in_pkt) + 1;
  test_ctx->total_pkts_in_frame = ops->height * pkts_in_line;
  test_ctx->pkt_idx = 0;
  test_ctx->seq_id = 1;
  test_ctx->pkts_in_line = pkts_in_line;
  test_ctx->width = ops->width;
  int pixels_in_pkts = (ops->width + pkts_in_line - 1) / pkts_in_line;
  int pkt_data_len = (pixels_in_pkts + test_ctx->st20_pg.coverage - 1) /
                     test_ctx->st20_pg.coverage * test_ctx->st20_pg.size;
  ops->rtp_frame_total_pkts = test_ctx->total_pkts_in_frame;
  ops->rtp_pkt_size = pkt_data_len + sizeof(struct st20_rfc4175_rtp_hdr);
  ops->notify_rtp_done = tx_rtp_done;
  ops->rtp_ring_size = 1024;
  test_ctx->pkt_data_len = pkt_data_len;
}

static void st20_tx_fps_test(enum st20_type type[], enum st_fps fps[], int width[],
                             int height[], enum st20_fmt fmt, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops;

  std::vector<tests_context*> test_ctx;
  std::vector<st20_tx_handle> handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread;

  test_ctx.resize(sessions);
  handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx[i] = new tests_context();
    ASSERT_TRUE(test_ctx[i] != NULL);

    test_ctx[i]->idx = i;
    test_ctx[i]->ctx = ctx;
    test_ctx[i]->fb_cnt = 3;
    test_ctx[i]->fb_idx = 0;
    st20_tx_ops_init(test_ctx[i], &ops);
    ops.type = type[i];
    ops.fps = fps[i];
    ops.width = width[i];
    ops.height = height[i];
    ops.fmt = fmt;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops, test_ctx[i]);
    }
    handle[i] = st20_tx_create(m_handle, &ops);
    test_ctx[i]->handle = handle[i];
    ASSERT_TRUE(handle[i] != NULL);
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = false;
      rtp_thread[i] = std::thread(tx_feed_packet, test_ctx[i]);
    }
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  int second = ctx->para.num_ports == 2 ? 10 : 5;
  sleep(second);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx[i]->fb_send / time_sec;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx[i]->mtx);
        test_ctx[i]->cv.notify_all();
      }
      rtp_thread[i].join();
    }
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_TRUE(test_ctx[i]->fb_send > 0);
    info("%s, session %d fb_send %d framerate %f\n", __func__, i, test_ctx[i]->fb_send,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st20_tx_free(handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx[i];
  }
}

static void st20_rx_fps_test(enum st20_type type[], enum st_fps fps[], int width[],
                             int height[], enum st20_fmt fmt, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 10000 + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_frame;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = 10000 + i;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = type[i];
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      test_ctx_rx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      {
        std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
        test_ctx_rx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
      rtp_thread_rx[i].join();
    }
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_tx, frame_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_tx, frame_1080p_fps29_97_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_tx, frame_1080p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_tx, frame_720p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1280};
  int height[1] = {720};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_tx, frame_1080p_yuv422_8bit_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_8BIT);
}
TEST(St20_tx, frame_1080p_yuv420_10bit_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_420_10BIT);
}
TEST(St20_tx, frame_1080p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_tx, frame_720p_fps29_97_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  int width[3] = {1280, 1280, 1280};
  int height[3] = {720, 720, 720};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_tx, frame_1080p_fps50_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_tx, frame_1080p_fps50_fps29_97) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P29_97};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_tx, frame_1080p_fps50_fps59_94) {
  enum st20_type type[2] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P50, ST_FPS_P59_94};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_tx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_1080p_fps50_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_rx, frame_1080p_fps50_s3) {
  enum st20_type type[3] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P50, ST_FPS_P50, ST_FPS_P50};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_rx, frame_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_rx, frame_1080p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_rx, frame_1080p_fps29_97_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT);
}
TEST(St20_rx, frame_1080p_fps29_97_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[3] = {ST_FPS_P29_97, ST_FPS_P29_97, ST_FPS_P29_97};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 3);
}
TEST(St20_rx, frame_1080p_fps29_97_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_1080p_fps59_94_fp50) {
  enum st20_type type[2] = {ST20_TYPE_RTP_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1920, 1920};
  int height[2] = {1080, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_1080p_fps29_97_720p_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P29_97, ST_FPS_P50};
  int width[2] = {1920, 1280};
  int height[2] = {1080, 720};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_720p_fps59_94_1080p_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 1920};
  int height[2] = {720, 1080};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}
TEST(St20_rx, frame_720p_fps59_94_4k_fp50) {
  enum st20_type type[2] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[2] = {ST_FPS_P59_94, ST_FPS_P50};
  int width[2] = {1280, 3840};
  int height[2] = {720, 2160};
  st20_rx_fps_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 2);
}

static void st20_rx_update_src_test(enum st20_type type, int tx_sessions) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }
  ASSERT_TRUE(tx_sessions >= 1);

  int rx_sessions = 1;

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(tx_sessions);
  test_ctx_rx.resize(rx_sessions);
  tx_handle.resize(tx_sessions);
  rx_handle.resize(rx_sessions);
  expect_framerate.resize(rx_sessions);
  framerate.resize(rx_sessions);
  rtp_thread_tx.resize(tx_sessions);
  rtp_thread_rx.resize(rx_sessions);

  for (int i = 0; i < rx_sessions; i++)
    expect_framerate[i] = st_frame_rate(ST_FPS_P59_94);

  for (int i = 0; i < tx_sessions; i++) {
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    if (2 == i)
      memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    else if (1 == i)
      memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    else
      memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 10000 + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = type;
    ops_tx.width = 1920;
    ops_tx.height = 1080;
    ops_tx.fps = ST_FPS_P59_94;
    ops_tx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_frame;
    if (type == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }

    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);

    if (type == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < rx_sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = 10000 + i;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = type;
    ops_rx.width = 1920;
    ops_rx.height = 1080;
    ops_rx.fps = ST_FPS_P59_94;
    ops_rx.fmt = ST20_FMT_YUV_422_10BIT;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    ops_rx.flags = ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    ASSERT_TRUE(rx_handle[i] != NULL);
    if (type == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
    }
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(2);

  struct st_rx_source_info src;
  /* switch to mcast port p(tx_session:1) */
  memset(&src, 0, sizeof(src));
  src.udp_port[ST_PORT_P] = 10000 + 1;
  memcpy(src.sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  test_ctx_tx[1]->seq_id = 0; /* reset seq id */
  for (int i = 0; i < rx_sessions; i++) {
    ret = st20_rx_update_source(rx_handle[i], &src);
    EXPECT_GE(ret, 0);
    test_ctx_rx[i]->start_time = 0;
    test_ctx_rx[i]->fb_rec = 0;
  }
  sleep(10);
  /* check rx fps */
  for (int i = 0; i < rx_sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
    info("%s, session %d fb_rec %d framerate %f for mcast 1\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    if (type == ST20_TYPE_FRAME_LEVEL) {
      EXPECT_TRUE(test_ctx_rx[i]->rtp_delta <=
                  3003);  // 3003 is for 59.94 fps rtp delta for 2 frames.
    }
  }

  if (tx_sessions > 2) {
    /* switch to mcast port r(tx_session:2) */
    memset(&src, 0, sizeof(src));
    src.udp_port[ST_PORT_P] = 10000 + 2;
    memcpy(src.sip_addr[ST_PORT_P], ctx->mcast_ip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    test_ctx_tx[2]->seq_id = rand(); /* random seq id */
    for (int i = 0; i < rx_sessions; i++) {
      ret = st20_rx_update_source(rx_handle[i], &src);
      EXPECT_GE(ret, 0);
      test_ctx_rx[i]->start_time = 0;
      test_ctx_rx[i]->fb_rec = 0;
    }
    sleep(10);
    /* check rx fps */
    for (int i = 0; i < rx_sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

      EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
      info("%s, session %d fb_rec %d framerate %f for mcast 2\n", __func__, i,
           test_ctx_rx[i]->fb_rec, framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
      if (type == ST20_TYPE_FRAME_LEVEL) {
        EXPECT_TRUE(test_ctx_rx[i]->rtp_delta <=
                    3003);  // 3003 is for 59.94 fps rtp delta for 2 frames.
      }
    }
  }

  /* switch to unicast(tx_session:0) */
  memset(&src, 0, sizeof(src));
  src.udp_port[ST_PORT_P] = 10000 + 0;
  memcpy(src.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
  test_ctx_tx[0]->seq_id = rand(); /* random seq id */
  for (int i = 0; i < rx_sessions; i++) {
    ret = st20_rx_update_source(rx_handle[i], &src);
    EXPECT_GE(ret, 0);
    test_ctx_rx[i]->start_time = 0;
    test_ctx_rx[i]->fb_rec = 0;
  }
  sleep(10);
  /* check rx fps */
  for (int i = 0; i < rx_sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
    info("%s, session %d fb_rec %d framerate %f for unicast 0\n", __func__, i,
         test_ctx_rx[i]->fb_rec, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    if (type == ST20_TYPE_FRAME_LEVEL) {
      EXPECT_TRUE(test_ctx_rx[i]->rtp_delta <=
                  3003);  // 3003 is for 59.94 fps rtp delta for 2 frames.
    }
  }

  /* stop rtp thread */
  for (int i = 0; i < rx_sessions; i++) {
    if (type == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
        test_ctx_rx[i]->cv.notify_all();
      }
      rtp_thread_rx[i].join();
    }
  }
  for (int i = 0; i < tx_sessions; i++) {
    if (type == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
    }
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);

  /* free all tx and rx */
  for (int i = 0; i < rx_sessions; i++) {
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_rx[i];
  }
  for (int i = 0; i < tx_sessions; i++) {
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
  }
}

TEST(St20_rx, update_source_frame) { st20_rx_update_src_test(ST20_TYPE_FRAME_LEVEL, 3); }
TEST(St20_rx, update_source_rtp) { st20_rx_update_src_test(ST20_TYPE_RTP_LEVEL, 2); }

static int st20_digest_rx_frame_ready(void* priv, void* frame,
                                      struct st20_frame_meta* meta) {
  auto ctx = (tests_context*)priv;

  if (!ctx->handle) return -EIO;

  if (!st20_is_frame_complete(meta->status)) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }
  if (meta->frame_total_size != ctx->frame_size) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }
  if (meta->frame_total_size != meta->frame_recv_size) {
    ctx->incomplete_frame_cnt++;
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    return 0;
  }

  std::unique_lock<std::mutex> lck(ctx->mtx);
  if (ctx->buf_q.empty()) {
    ctx->buf_q.push(frame);
    ctx->cv.notify_all();
  } else {
    st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
  }
  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  dbg("%s, frame %p\n", __func__, frame);
  return 0;
}

static void st20_digest_rx_frame_check(void* args) {
  auto ctx = (tests_context*)args;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  unsigned char result[MD5_DIGEST_LENGTH];
  while (!ctx->stop) {
    if (ctx->buf_q.empty()) {
      lck.lock();
      if (!ctx->stop) ctx->cv.wait(lck);
      lck.unlock();
      continue;
    } else {
      void* frame = ctx->buf_q.front();
      ctx->buf_q.pop();
      dbg("%s, frame %p\n", __func__, frame);
      int i;
      MD5((unsigned char*)frame, ctx->frame_size, result);
      for (i = 0; i < TEST_MD5_HIST_NUM; i++) {
        unsigned char* target_md5 = ctx->md5s[i];
        if (!memcmp(result, target_md5, MD5_DIGEST_LENGTH)) break;
      }
      if (i >= TEST_MD5_HIST_NUM) {
        test_md5_dump("st20_rx_error_md5", result);
        ctx->fail_cnt++;
      }
      ctx->check_md5_frame_cnt++;
      st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);
    }
  }
}

static void st20_rx_digest_test(enum st20_type type[], enum st20_packing packing[],
                                enum st_fps fps[], int width[], int height[],
                                enum st20_fmt fmt, bool check_fps, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;
  std::vector<std::thread> md5_check;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);
  md5_check.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = TEST_MD5_HIST_NUM;
    test_ctx_tx[i]->fb_idx = 0;
    test_ctx_tx[i]->check_md5 = true;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_digest_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 10000 + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.packing = packing[i];
    ops_tx.type = type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_frame;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);

    /* md5 caculate */
    struct st20_pgroup st20_pg;
    st20_get_pgroup(ops_tx.fmt, &st20_pg);
    size_t frame_size = ops_tx.width * ops_tx.height * st20_pg.size / st20_pg.coverage;
    test_ctx_tx[i]->frame_size = frame_size;
    uint8_t* fb;
    for (int frame = 0; frame < TEST_MD5_HIST_NUM; frame++) {
      if (type[i] == ST20_TYPE_FRAME_LEVEL) {
        fb = (uint8_t*)st20_tx_get_framebuffer(tx_handle[i], frame);
      } else {
        test_ctx_tx[i]->frame_buf[frame] = (uint8_t*)st_test_zmalloc(frame_size);
        fb = test_ctx_tx[i]->frame_buf[frame];
      }
      ASSERT_TRUE(fb != NULL);
      for (size_t s = 0; s < frame_size; s++) fb[s] = rand() % 0xFF;
      unsigned char* result = test_ctx_tx[i]->md5s[frame];
      MD5((unsigned char*)fb, frame_size, result);
      test_md5_dump("st20_rx", result);
    }
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    test_ctx_rx[i]->check_md5 = true;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_digest_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = 10000 + i;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = type[i];
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_digest_rx_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    test_ctx_rx[i]->width = ops_rx.width;
    st20_get_pgroup(ops_rx.fmt, &test_ctx_rx[i]->st20_pg);
    memcpy(test_ctx_rx[i]->md5s, test_ctx_tx[i]->md5s,
           TEST_MD5_HIST_NUM * MD5_DIGEST_LENGTH);
    ASSERT_TRUE(rx_handle[i] != NULL);
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      md5_check[i] = std::thread(md5_frame_check, test_ctx_rx[i]);
    } else {
      test_ctx_rx[i]->stop = false;
      rtp_thread_rx[i] = std::thread(st20_digest_rx_frame_check, test_ctx_rx[i]);
    }
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10 * 1);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
    }
    test_ctx_rx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
      test_ctx_rx[i]->cv.notify_all();
    }
    rtp_thread_rx[i].join();
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      md5_check[i].join();
      while (!test_ctx_rx[i]->buf_q.empty()) {
        void* frame = test_ctx_rx[i]->buf_q.front();
        st_test_free(frame);
        test_ctx_rx[i]->buf_q.pop();
      }
    }
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GE(test_ctx_rx[i]->fb_rec, 0);
    EXPECT_GE(test_ctx_rx[i]->check_md5_frame_cnt, 0);
    EXPECT_EQ(test_ctx_rx[i]->incomplete_frame_cnt, 0);
    if (type[i] == ST20_TYPE_FRAME_LEVEL)
      EXPECT_EQ(test_ctx_rx[i]->fail_cnt, 0);
    else
      EXPECT_TRUE(test_ctx_rx[i]->fail_cnt < 2);
    info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
         framerate[i]);
    if (check_fps) {
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    }
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      for (int frame = 0; frame < TEST_MD5_HIST_NUM; frame++) {
        if (test_ctx_tx[i]->frame_buf[frame])
          st_test_free(test_ctx_tx[i]->frame_buf[frame]);
        if (test_ctx_rx[i]->frame_buf[frame])
          st_test_free(test_ctx_rx[i]->frame_buf[frame]);
      }
    }
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_rx, digest_frame_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_GPM_SL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_digest_test(type, packing, fps, width, height, ST20_FMT_YUV_422_10BIT, true);
}

TEST(St20_rx, digest_rtp_1080p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_GPM_SL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_digest_test(type, packing, fps, width, height, ST20_FMT_YUV_422_10BIT, true);
}

TEST(St20_rx, digest_frame_4320p_fps59_94_s1) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[1] = {ST20_PACKING_GPM_SL};
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920 * 4};
  int height[1] = {1080 * 4};
  st20_rx_digest_test(type, packing, fps, width, height, ST20_FMT_YUV_422_10BIT, false);
}

TEST(St20_rx, digest_frame_720p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1280, 1280, 1280};
  int height[3] = {720, 720, 720};
  st20_rx_digest_test(type, packing, fps, width, height, ST20_FMT_YUV_422_10BIT, false,
                      3);
}

TEST(St20_rx, digest_frame_1080p_fps59_94_s3) {
  enum st20_type type[3] = {ST20_TYPE_FRAME_LEVEL, ST20_TYPE_FRAME_LEVEL,
                            ST20_TYPE_FRAME_LEVEL};
  enum st20_packing packing[3] = {ST20_PACKING_GPM_SL, ST20_PACKING_GPM,
                                  ST20_PACKING_BPM};
  enum st_fps fps[3] = {ST_FPS_P59_94, ST_FPS_P59_94, ST_FPS_P59_94};
  int width[3] = {1920, 1920, 1920};
  int height[3] = {1080, 1080, 1080};
  st20_rx_digest_test(type, packing, fps, width, height, ST20_FMT_YUV_422_10BIT, false,
                      3);
}

static int st20_tx_meta_build_rtp(tests_context* s, struct st20_rfc4175_rtp_hdr* rtp,
                                  uint16_t* pkt_len) {
  uint16_t data_len = s->pkt_data_len;
  int pkts_in_line = s->pkts_in_line;
  int row_number = s->pkt_idx / pkts_in_line;
  int pixels_in_pkt = s->pkt_data_len / s->st20_pg.size * s->st20_pg.coverage;
  int row_offset = pixels_in_pkt * (s->pkt_idx % pkts_in_line);
  bool marker = false;

  /* update hdr */
  rtp->base.csrc_count = 0;
  rtp->base.extension = 0;
  rtp->base.padding = 0;
  rtp->base.version = 2;
  rtp->base.marker = 0;
  rtp->base.payload_type = 96;
  rtp->row_number = htons(row_number);
  rtp->row_offset = htons(row_offset);
  rtp->base.tmstamp = htonl(s->rtp_tmstamp);
  rtp->base.seq_number = htons(s->seq_id);
  rtp->seq_number_ext = htons((uint16_t)(s->seq_id >> 16));
  s->seq_id++;
  int temp = (s->width - row_offset) / s->st20_pg.coverage * s->st20_pg.size;
  data_len = s->pkt_data_len > temp ? temp : s->pkt_data_len;
  rtp->row_length = htons(data_len);
  *pkt_len = data_len + sizeof(*rtp);

  s->pkt_idx++;

  /* build incomplete frame */
  if (s->pkt_idx >= s->total_pkts_in_frame) marker = true;
  if (s->fb_send % 2) {
    if (s->pkt_idx >= (s->total_pkts_in_frame / 2)) marker = true;
  }
  if (marker) {
    /* end of current frame */
    rtp->base.marker = 1;

    s->pkt_idx = 0;
    s->rtp_tmstamp++;
    s->fb_send++;
  }

  return 0;
}

static void st20_rx_meta_feed_packet(void* args) {
  auto ctx = (tests_context*)args;
  void* mbuf;
  void* usrptr = NULL;
  uint16_t mbuf_len = 0;
  std::unique_lock<std::mutex> lck(ctx->mtx, std::defer_lock);
  while (!ctx->stop) {
    /* get available buffer*/
    mbuf = st20_tx_get_mbuf(ctx->handle, &usrptr);
    if (!mbuf) {
      lck.lock();
      /* try again */
      mbuf = st20_tx_get_mbuf(ctx->handle, &usrptr);
      if (mbuf) {
        lck.unlock();
      } else {
        if (!ctx->stop) ctx->cv.wait(lck);
        lck.unlock();
        continue;
      }
    }

    /* build the rtp pkt */
    st20_tx_meta_build_rtp(ctx, (struct st20_rfc4175_rtp_hdr*)usrptr, &mbuf_len);

    st20_tx_put_mbuf(ctx->handle, mbuf, mbuf_len);
  }
}

static int st20_rx_meta_frame_ready(void* priv, void* frame,
                                    struct st20_frame_meta* meta) {
  auto ctx = (tests_context*)priv;
  auto expect_meta = (struct st20_frame_meta*)ctx->priv;

  if (!ctx->handle) return -EIO;

  ctx->fb_rec++;
  if (!ctx->start_time) ctx->start_time = st_test_get_monotonic_time();
  if (expect_meta->width != meta->width) ctx->fail_cnt++;
  if (expect_meta->height != meta->height) ctx->fail_cnt++;
  if (expect_meta->fps != meta->fps) ctx->fail_cnt++;
  if (expect_meta->fmt != meta->fmt) ctx->fail_cnt++;
  if (expect_meta->timestamp == meta->timestamp) ctx->fail_cnt++;
  expect_meta->timestamp = meta->timestamp;
  if (!st20_is_frame_complete(meta->status)) {
    ctx->incomplete_frame_cnt++;
    if (meta->frame_total_size <= meta->frame_recv_size) ctx->fail_cnt++;
  } else {
    if (meta->frame_total_size != meta->frame_recv_size) ctx->fail_cnt++;
  }
  st20_rx_put_framebuff((st20_rx_handle)ctx->handle, frame);

  return 0;
}

static void st20_rx_meta_test(enum st_fps fps[], int width[], int height[],
                              enum st20_fmt fmt, int sessions = 1) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_meta_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 10000 + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = ST20_TYPE_RTP_LEVEL;
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_frame;
    rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);
    test_ctx_tx[i]->stop = false;
    rtp_thread_tx[i] = std::thread(st20_rx_meta_feed_packet, test_ctx_tx[i]);
  }

  for (int i = 0; i < sessions; i++) {
    test_ctx_rx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_rx[i] != NULL);

    test_ctx_rx[i]->idx = i;
    test_ctx_rx[i]->ctx = ctx;
    test_ctx_rx[i]->fb_cnt = 3;
    test_ctx_rx[i]->fb_idx = 0;
    memset(&ops_rx, 0, sizeof(ops_rx));
    ops_rx.name = "st20_meta_test";
    ops_rx.priv = test_ctx_rx[i];
    ops_rx.num_port = 1;
    memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
    strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
    ops_rx.udp_port[ST_PORT_P] = 10000 + i;
    ops_rx.pacing = ST21_PACING_NARROW;
    ops_rx.type = ST20_TYPE_FRAME_LEVEL;
    ops_rx.width = width[i];
    ops_rx.height = height[i];
    ops_rx.fps = fps[i];
    ops_rx.fmt = fmt;
    ops_rx.flags = ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
    ops_rx.notify_frame_ready = st20_rx_meta_frame_ready;
    ops_rx.notify_rtp_ready = rx_rtp_ready;
    ops_rx.rtp_ring_size = 1024;
    rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
    test_ctx_rx[i]->handle = rx_handle[i];
    test_ctx_rx[i]->frame_size = test_ctx_tx[i]->frame_size;
    ASSERT_TRUE(rx_handle[i] != NULL);
    test_ctx_rx[i]->stop = false;

    /* set expect meta data to private */
    auto meta = (struct st20_frame_meta*)st_test_zmalloc(sizeof(struct st20_frame_meta));
    ASSERT_TRUE(meta != NULL);
    meta->width = ops_rx.width;
    meta->height = ops_rx.height;
    meta->fps = ops_rx.fps;
    meta->fmt = ops_rx.fmt;
    test_ctx_rx[i]->priv = meta;
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(10);

  for (int i = 0; i < sessions; i++) {
    uint64_t cur_time_ns = st_test_get_monotonic_time();
    double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
    framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

    /* stop all thread */
    test_ctx_tx[i]->stop = true;
    {
      std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
      test_ctx_tx[i]->cv.notify_all();
    }
    rtp_thread_tx[i].join();

    test_ctx_rx[i]->stop = true;
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  for (int i = 0; i < sessions; i++) {
    EXPECT_GE(test_ctx_rx[i]->fb_rec, 0);
    float expect_incomplete_frame_cnt = test_ctx_rx[i]->fb_rec / 2;
    EXPECT_NEAR(test_ctx_rx[i]->incomplete_frame_cnt, expect_incomplete_frame_cnt,
                expect_incomplete_frame_cnt * 0.1);
    EXPECT_EQ(test_ctx_rx[i]->fail_cnt, 0);
    info("%s, session %d fb_rec %d fb_incomplete %d framerate %f\n", __func__, i,
         test_ctx_rx[i]->fb_rec, test_ctx_rx[i]->incomplete_frame_cnt, framerate[i]);
    EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    ret = st20_rx_free(rx_handle[i]);
    EXPECT_GE(ret, 0);
    st_test_free(test_ctx_rx[i]->priv);
    delete test_ctx_tx[i];
    delete test_ctx_rx[i];
  }
}

TEST(St20_rx, frame_meta_1080p_fps59_94_s1) {
  enum st_fps fps[1] = {ST_FPS_P59_94};
  int width[1] = {1920};
  int height[1] = {1080};
  st20_rx_meta_test(fps, width, height, ST20_FMT_YUV_422_10BIT);
}

static void st20_rx_after_start_test(enum st20_type type[], enum st_fps fps[],
                                     int width[], int height[], enum st20_fmt fmt,
                                     int sessions, int repeat) {
  auto ctx = (struct st_tests_context*)st_test_ctx();
  auto m_handle = ctx->handle;
  int ret;
  struct st20_tx_ops ops_tx;
  struct st20_rx_ops ops_rx;
  if (ctx->para.num_ports != 2) {
    info("%s, dual port should be enabled for tx test, one for tx and one for rx\n",
         __func__);
    return;
  }

  std::vector<tests_context*> test_ctx_tx;
  std::vector<tests_context*> test_ctx_rx;
  std::vector<st20_tx_handle> tx_handle;
  std::vector<st20_rx_handle> rx_handle;
  std::vector<double> expect_framerate;
  std::vector<double> framerate;
  std::vector<std::thread> rtp_thread_tx;
  std::vector<std::thread> rtp_thread_rx;

  test_ctx_tx.resize(sessions);
  test_ctx_rx.resize(sessions);
  tx_handle.resize(sessions);
  rx_handle.resize(sessions);
  expect_framerate.resize(sessions);
  framerate.resize(sessions);
  rtp_thread_tx.resize(sessions);
  rtp_thread_rx.resize(sessions);

  for (int i = 0; i < sessions; i++) {
    expect_framerate[i] = st_frame_rate(fps[i]);
    test_ctx_tx[i] = new tests_context();
    ASSERT_TRUE(test_ctx_tx[i] != NULL);

    test_ctx_tx[i]->idx = i;
    test_ctx_tx[i]->ctx = ctx;
    test_ctx_tx[i]->fb_cnt = 3;
    test_ctx_tx[i]->fb_idx = 0;
    memset(&ops_tx, 0, sizeof(ops_tx));
    ops_tx.name = "st20_test";
    ops_tx.priv = test_ctx_tx[i];
    ops_tx.num_port = 1;
    memcpy(ops_tx.dip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_R], ST_IP_ADDR_LEN);
    strncpy(ops_tx.port[ST_PORT_P], ctx->para.port[ST_PORT_P], ST_PORT_MAX_LEN);
    ops_tx.udp_port[ST_PORT_P] = 10000 + i;
    ops_tx.pacing = ST21_PACING_NARROW;
    ops_tx.type = type[i];
    ops_tx.width = width[i];
    ops_tx.height = height[i];
    ops_tx.fps = fps[i];
    ops_tx.fmt = fmt;
    ops_tx.framebuff_cnt = test_ctx_tx[i]->fb_cnt;
    ops_tx.get_next_frame = tx_next_frame;
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      rtp_tx_specific_init(&ops_tx, test_ctx_tx[i]);
    }
    tx_handle[i] = st20_tx_create(m_handle, &ops_tx);
    test_ctx_tx[i]->handle = tx_handle[i];
    ASSERT_TRUE(tx_handle[i] != NULL);
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = false;
      rtp_thread_tx[i] = std::thread(tx_feed_packet, test_ctx_tx[i]);
    }
  }

  ret = st_start(m_handle);
  EXPECT_GE(ret, 0);
  sleep(5);

  for (int r = 0; r < repeat; r++) {
    for (int i = 0; i < sessions; i++) {
      test_ctx_rx[i] = new tests_context();
      ASSERT_TRUE(test_ctx_rx[i] != NULL);

      test_ctx_rx[i]->idx = i;
      test_ctx_rx[i]->ctx = ctx;
      test_ctx_rx[i]->fb_cnt = 3;
      test_ctx_rx[i]->fb_idx = 0;
      memset(&ops_rx, 0, sizeof(ops_rx));
      ops_rx.name = "st20_test";
      ops_rx.priv = test_ctx_rx[i];
      ops_rx.num_port = 1;
      memcpy(ops_rx.sip_addr[ST_PORT_P], ctx->para.sip_addr[ST_PORT_P], ST_IP_ADDR_LEN);
      strncpy(ops_rx.port[ST_PORT_P], ctx->para.port[ST_PORT_R], ST_PORT_MAX_LEN);
      ops_rx.udp_port[ST_PORT_P] = 10000 + i;
      ops_rx.pacing = ST21_PACING_NARROW;
      ops_rx.type = type[i];
      ops_rx.width = width[i];
      ops_rx.height = height[i];
      ops_rx.fps = fps[i];
      ops_rx.fmt = fmt;
      ops_rx.framebuff_cnt = test_ctx_rx[i]->fb_cnt;
      ops_rx.notify_frame_ready = st20_rx_frame_ready;
      ops_rx.notify_rtp_ready = rx_rtp_ready;
      ops_rx.rtp_ring_size = 1024;
      rx_handle[i] = st20_rx_create(m_handle, &ops_rx);
      test_ctx_rx[i]->handle = rx_handle[i];
      ASSERT_TRUE(rx_handle[i] != NULL);
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        test_ctx_rx[i]->stop = false;
        rtp_thread_rx[i] = std::thread(rx_get_packet, test_ctx_rx[i]);
      }
    }

    sleep(10);

    for (int i = 0; i < sessions; i++) {
      uint64_t cur_time_ns = st_test_get_monotonic_time();
      double time_sec = (double)(cur_time_ns - test_ctx_rx[i]->start_time) / NS_PER_S;
      framerate[i] = test_ctx_rx[i]->fb_rec / time_sec;

      /* stop rx rtp if */
      if (type[i] == ST20_TYPE_RTP_LEVEL) {
        test_ctx_rx[i]->stop = true;
        {
          std::unique_lock<std::mutex> lck(test_ctx_rx[i]->mtx);
          test_ctx_rx[i]->cv.notify_all();
        }
        rtp_thread_rx[i].join();
      }
    }

    for (int i = 0; i < sessions; i++) {
      EXPECT_TRUE(test_ctx_rx[i]->fb_rec > 0);
      info("%s, session %d fb_rec %d framerate %f\n", __func__, i, test_ctx_rx[i]->fb_rec,
           framerate[i]);
      EXPECT_NEAR(framerate[i], expect_framerate[i], expect_framerate[i] * 0.1);
      ret = st20_rx_free(rx_handle[i]);
      EXPECT_GE(ret, 0);
      delete test_ctx_rx[i];
    }

    sleep(2);
  }

  /* stop tx rtp if */
  for (int i = 0; i < sessions; i++) {
    if (type[i] == ST20_TYPE_RTP_LEVEL) {
      test_ctx_tx[i]->stop = true;
      {
        std::unique_lock<std::mutex> lck(test_ctx_tx[i]->mtx);
        test_ctx_tx[i]->cv.notify_all();
      }
      rtp_thread_tx[i].join();
    }
  }

  ret = st_stop(m_handle);
  EXPECT_GE(ret, 0);
  /* free all tx */
  for (int i = 0; i < sessions; i++) {
    ret = st20_tx_free(tx_handle[i]);
    EXPECT_GE(ret, 0);
    delete test_ctx_tx[i];
  }
}

TEST(St20_rx, after_start_frame_720p_fps50_s1_r1) {
  enum st20_type type[1] = {ST20_TYPE_RTP_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P50};
  int width[1] = {1280};
  int height[1] = {720};
  st20_rx_after_start_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 1, 1);
}

TEST(St20_rx, after_start_frame_720p_fps29_97_s1_r2) {
  enum st20_type type[1] = {ST20_TYPE_FRAME_LEVEL};
  enum st_fps fps[1] = {ST_FPS_P29_97};
  int width[1] = {1280};
  int height[1] = {720};
  st20_rx_after_start_test(type, fps, width, height, ST20_FMT_YUV_422_10BIT, 1, 2);
}
