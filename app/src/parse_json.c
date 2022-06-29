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

#include "parse_json.h"

#include "log.h"

#if (JSON_C_VERSION_NUM >= ((0 << 16) | (13 << 8) | 0)) || \
    (JSON_C_VERSION_NUM < ((0 << 16) | (10 << 8) | 0))
static inline json_object* st_json_object_object_get(json_object* obj, const char* key) {
  return json_object_object_get(obj, key);
}
#else
static inline json_object* st_json_object_object_get(json_object* obj, const char* key) {
  json_object* value;
  int ret = json_object_object_get_ex(obj, key, &value);
  if (ret) return value;
  err("%s, can not get object with key: %s!\n", __func__, key);
  return NULL;
}
#endif

#define VNAME(name) (#name)

#define CHECK_STRING(string)                                  \
  do {                                                        \
    if (string == NULL) {                                     \
      err("%s, can not parse %s\n", __func__, VNAME(string)); \
      return -ST_JSON_PARSE_FAIL;                             \
    }                                                         \
  } while (0)

/* 7 bits payload type define in RFC3550 */
static inline bool st_json_is_valid_payload_type(int payload_type) {
  if (payload_type > 0 && payload_type < 0x7F)
    return true;
  else
    return false;
}

static int st_json_parse_interfaces(json_object* interface_obj,
                                    st_json_interface_t* interface) {
  if (interface_obj == NULL || interface == NULL) {
    err("%s, can not parse interfaces!\n", __func__);
    return -ST_JSON_NULL;
  }

  const char* name =
      json_object_get_string(st_json_object_object_get(interface_obj, "name"));
  CHECK_STRING(name);
  snprintf(interface->name, sizeof(interface->name), "%s", name);

  const char* ip = json_object_get_string(st_json_object_object_get(interface_obj, "ip"));
  CHECK_STRING(ip);
  inet_pton(AF_INET, ip, interface->ip_addr);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_video(int idx, json_object* video_obj,
                                  st_json_tx_video_session_t* video) {
  if (video_obj == NULL || video == NULL) {
    err("%s, can not parse tx video session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse video type */
  const char* type = json_object_get_string(st_json_object_object_get(video_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    video->type = ST20_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    video->type = ST20_TYPE_RTP_LEVEL;
  } else if (strcmp(type, "slice") == 0) {
    video->type = ST20_TYPE_SLICE_LEVEL;
  } else {
    err("%s, invalid video type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video pacing */
  const char* pacing =
      json_object_get_string(st_json_object_object_get(video_obj, "pacing"));
  CHECK_STRING(pacing);
  if (strcmp(pacing, "gap") == 0) {
    video->pacing = PACING_GAP;
  } else if (strcmp(pacing, "linear") == 0) {
    video->pacing = PACING_LINEAR;
  } else {
    err("%s, invalid video pacing %s\n", __func__, pacing);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video packing mode */
  const char* packing =
      json_object_get_string(st_json_object_object_get(video_obj, "packing"));
  // CHECK_STRING(packing);
  if (packing) {
    if (strcmp(packing, "GPM_SL") == 0) {
      video->packing = ST20_PACKING_GPM_SL;
    } else if (strcmp(packing, "BPM") == 0) {
      video->packing = ST20_PACKING_BPM;
    } else if (strcmp(packing, "GPM") == 0) {
      video->packing = ST20_PACKING_GPM;
    } else {
      err("%s, invalid video packing mode %s\n", __func__, packing);
      return -ST_JSON_NOT_VALID;
    }
  } else {
    video->packing = ST20_PACKING_BPM;
  }

  /* parse udp port */
  int start_port =
      json_object_get_int(st_json_object_object_get(video_obj, "start_port"));
  if (start_port <= 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  video->udp_port = start_port + idx;

  /* parse payload type */
  json_object* payload_type_object = st_json_object_object_get(video_obj, "payload_type");
  int payload_type = ST_APP_PAYLOAD_TYPE_VIDEO;
  if (payload_type_object) {
    payload_type = json_object_get_int(payload_type_object);
    if (!st_json_is_valid_payload_type(payload_type)) {
      err("%s, invalid payload type %d\n", __func__, payload_type);
      return -ST_JSON_NOT_VALID;
    }
  }
  video->payload_type = payload_type;

  /* parse tr offset */
  const char* tr_offset =
      json_object_get_string(st_json_object_object_get(video_obj, "tr_offset"));
  CHECK_STRING(tr_offset);
  if (strcmp(tr_offset, "default") == 0) {
    video->tr_offset = TR_OFFSET_DEFAULT;
  } else if (strcmp(pacing, "none") == 0) {
    video->tr_offset = TR_OFFSET_NONE;
  } else {
    err("%s, invalid video tr_offset %s\n", __func__, tr_offset);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video format */
  const char* video_format =
      json_object_get_string(st_json_object_object_get(video_obj, "video_format"));
  CHECK_STRING(video_format);
  if (strcmp(video_format, "i1080p59") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_59FPS;
  } else if (strcmp(video_format, "i1080p50") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_50FPS;
  } else if (strcmp(video_format, "i1080p29") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_29FPS;
  } else if (strcmp(video_format, "i1080p25") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_25FPS;
  } else if (strcmp(video_format, "i2160p59") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_59FPS;
  } else if (strcmp(video_format, "i2160p50") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_50FPS;
  } else if (strcmp(video_format, "i2160p29") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_29FPS;
  } else if (strcmp(video_format, "i2160p25") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_25FPS;
  } else if (strcmp(video_format, "i720p59") == 0) {
    video->video_format = VIDEO_FORMAT_720P_59FPS;
  } else if (strcmp(video_format, "i720p50") == 0) {
    video->video_format = VIDEO_FORMAT_720P_50FPS;
  } else if (strcmp(video_format, "i720p29") == 0) {
    video->video_format = VIDEO_FORMAT_720P_29FPS;
  } else if (strcmp(video_format, "i720p25") == 0) {
    video->video_format = VIDEO_FORMAT_720P_25FPS;
  } else if (strcmp(video_format, "i4320p59") == 0) {
    video->video_format = VIDEO_FORMAT_4320P_59FPS;
  } else if (strcmp(video_format, "i4320p50") == 0) {
    video->video_format = VIDEO_FORMAT_4320P_50FPS;
  } else if (strcmp(video_format, "i4320p29") == 0) {
    video->video_format = VIDEO_FORMAT_4320P_29FPS;
  } else if (strcmp(video_format, "i4320p25") == 0) {
    video->video_format = VIDEO_FORMAT_4320P_25FPS;
  } else if (strcmp(video_format, "i1080i59") == 0) {
    video->video_format = VIDEO_FORMAT_1080I_59FPS;
  } else if (strcmp(video_format, "i1080i50") == 0) {
    video->video_format = VIDEO_FORMAT_1080I_50FPS;
  } else if (strcmp(video_format, "i480i59") == 0) {
    video->video_format = VIDEO_FORMAT_480I_59FPS;
  } else if (strcmp(video_format, "i576i50") == 0) {
    video->video_format = VIDEO_FORMAT_576I_50FPS;
  } else {
    err("%s, invalid video format %s\n", __func__, video_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse pixel group format */
  const char* pg_format =
      json_object_get_string(st_json_object_object_get(video_obj, "pg_format"));
  CHECK_STRING(pg_format);
  if (strcmp(pg_format, "YUV_422_10bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_10BIT;
  } else if (strcmp(pg_format, "YUV_422_8bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_8BIT;
  } else if (strcmp(pg_format, "YUV_422_12bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_12BIT;
  } else if (strcmp(pg_format, "YUV_422_16bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_16BIT;
  } else if (strcmp(pg_format, "YUV_420_8bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_8BIT;
  } else if (strcmp(pg_format, "YUV_420_10bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_10BIT;
  } else if (strcmp(pg_format, "YUV_420_12bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_12BIT;
  } else if (strcmp(pg_format, "RGB_8bit") == 0) {
    video->pg_format = ST20_FMT_RGB_8BIT;
  } else if (strcmp(pg_format, "RGB_10bit") == 0) {
    video->pg_format = ST20_FMT_RGB_10BIT;
  } else if (strcmp(pg_format, "RGB_12bit") == 0) {
    video->pg_format = ST20_FMT_RGB_12BIT;
  } else if (strcmp(pg_format, "RGB_16bit") == 0) {
    video->pg_format = ST20_FMT_RGB_16BIT;
  } else {
    err("%s, invalid pixel group format %s\n", __func__, pg_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video url */
  const char* video_url =
      json_object_get_string(st_json_object_object_get(video_obj, "video_url"));
  CHECK_STRING(video_url);
  snprintf(video->video_url, sizeof(video->video_url), "%s", video_url);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_audio(int idx, json_object* audio_obj,
                                  st_json_tx_audio_session_t* audio) {
  if (audio_obj == NULL || audio == NULL) {
    err("%s, can not parse tx audio session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse audio type */
  const char* type = json_object_get_string(st_json_object_object_get(audio_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    audio->type = ST30_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    audio->type = ST30_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid audio type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio format */
  const char* audio_format =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_format"));
  CHECK_STRING(audio_format);
  if (strcmp(audio_format, "PCM8") == 0) {
    audio->audio_format = ST30_FMT_PCM8;
  } else if (strcmp(audio_format, "PCM16") == 0) {
    audio->audio_format = ST30_FMT_PCM16;
  } else if (strcmp(audio_format, "PCM24") == 0) {
    audio->audio_format = ST30_FMT_PCM24;
  } else if (strcmp(audio_format, "AM824") == 0) {
    audio->audio_format = ST31_FMT_AM824;
  } else {
    err("%s, invalid audio format %s\n", __func__, audio_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio channel */
  json_object* audio_channel_array =
      st_json_object_object_get(audio_obj, "audio_channel");
  if (audio_channel_array == NULL ||
      json_object_get_type(audio_channel_array) != json_type_array) {
    err("%s, can not parse audio channel\n", __func__);
    return -ST_JSON_PARSE_FAIL;
  }
  for (int i = 0; i < json_object_array_length(audio_channel_array); ++i) {
    json_object* channel_obj = json_object_array_get_idx(audio_channel_array, i);
    const char* channel = json_object_get_string(channel_obj);
    CHECK_STRING(channel);
    if (strcmp(channel, "M") == 0) {
      audio->audio_channel += 1;
    } else if (strcmp(channel, "DM") == 0 || strcmp(channel, "ST") == 0 ||
               strcmp(channel, "LtRt") == 0 || strcmp(channel, "AES3") == 0) {
      audio->audio_channel += 2;
    } else if (strcmp(channel, "51") == 0) {
      audio->audio_channel += 6;
    } else if (strcmp(channel, "71") == 0) {
      audio->audio_channel += 8;
    } else if (strcmp(channel, "222") == 0) {
      audio->audio_channel += 24;
    } else if (strcmp(channel, "SGRP") == 0) {
      audio->audio_channel += 4;
    } else if (channel[0] == 'U' && channel[1] >= '0' && channel[1] <= '9' &&
               channel[2] >= '0' && channel[2] <= '9' && channel[3] == '\0') {
      int num_channel = (channel[1] - '0') * 10 + (channel[2] - '0');
      if (num_channel < 1 || num_channel > 64) {
        err("%s, audio undefined channel number out of range %s\n", __func__, channel);
        return -ST_JSON_NOT_VALID;
      }
      audio->audio_channel += num_channel;
    } else {
      err("%s, invalid audio channel %s\n", __func__, channel);
      return -ST_JSON_NOT_VALID;
    }
  }

  /* parse audio sampling */
  const char* audio_sampling =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_sampling"));
  CHECK_STRING(audio_sampling);
  if (strcmp(audio_sampling, "48kHz") == 0) {
    audio->audio_sampling = ST30_SAMPLING_48K;
  } else if (strcmp(audio_sampling, "96kHz") == 0) {
    audio->audio_sampling = ST30_SAMPLING_96K;
  } else if (strcmp(audio_sampling, "44.1kHz") == 0) {
    audio->audio_sampling = ST31_SAMPLING_44K;
  } else {
    err("%s, invalid audio sampling %s\n", __func__, audio_sampling);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio packet time */
  const char* audio_ptime =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_ptime"));
  // CHECK_STRING(audio_ptime);
  if (audio_ptime) {
    if (strcmp(audio_ptime, "1") == 0) {
      audio->audio_ptime = ST30_PTIME_1MS;
    } else if (strcmp(audio_ptime, "0.12") == 0) {
      audio->audio_ptime = ST30_PTIME_125US;
    } else if (strcmp(audio_ptime, "0.08") == 0) {
      audio->audio_ptime = ST30_PTIME_80US;
    } else if (strcmp(audio_ptime, "1.09") == 0) {
      audio->audio_ptime = ST31_PTIME_1_09MS;
    } else if (strcmp(audio_ptime, "0.14") == 0) {
      audio->audio_ptime = ST31_PTIME_0_14MS;
    } else if (strcmp(audio_ptime, "0.09") == 0) {
      audio->audio_ptime = ST31_PTIME_0_09MS;
    } else {
      err("%s, invalid audio ptime %s\n", __func__, audio_ptime);
      return -ST_JSON_NOT_VALID;
    }
  } else {
    audio->audio_ptime = ST30_PTIME_1MS;
  }

  /* parse udp port */
  int start_port =
      json_object_get_int(st_json_object_object_get(audio_obj, "start_port"));
  if (start_port <= 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  audio->udp_port = start_port + idx;

  /* parse payload type */
  json_object* payload_type_object = st_json_object_object_get(audio_obj, "payload_type");
  int payload_type = ST_APP_PAYLOAD_TYPE_AUDIO;
  if (payload_type_object) {
    payload_type = json_object_get_int(payload_type_object);
    if (!st_json_is_valid_payload_type(payload_type)) {
      err("%s, invalid payload type %d\n", __func__, payload_type);
      return -ST_JSON_NOT_VALID;
    }
  }
  audio->payload_type = payload_type;

  /* parse audio url */
  const char* audio_url =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_url"));
  CHECK_STRING(audio_url);
  snprintf(audio->audio_url, sizeof(audio->audio_url), "%s", audio_url);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_anc(int idx, json_object* anc_obj,
                                st_json_tx_ancillary_session_t* anc) {
  if (anc_obj == NULL || anc == NULL) {
    err("%s, can not parse tx anc session\n", __func__);
    return -ST_JSON_NULL;
  }
  /* parse anc type */
  const char* type = json_object_get_string(st_json_object_object_get(anc_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    anc->type = ST40_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    anc->type = ST40_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid anc type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }
  /* parse anc format */
  const char* anc_format =
      json_object_get_string(st_json_object_object_get(anc_obj, "ancillary_format"));
  CHECK_STRING(anc_format);
  if (strcmp(anc_format, "closed_caption") == 0) {
    anc->anc_format = ANC_FORMAT_CLOSED_CAPTION;
  } else {
    err("%s, invalid anc format %s\n", __func__, anc_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse anc fps */
  const char* anc_fps =
      json_object_get_string(st_json_object_object_get(anc_obj, "ancillary_fps"));
  CHECK_STRING(anc_fps);
  if (strcmp(anc_fps, "p59") == 0) {
    anc->anc_fps = ST_FPS_P59_94;
  } else if (strcmp(anc_fps, "p50") == 0) {
    anc->anc_fps = ST_FPS_P50;
  } else if (strcmp(anc_fps, "p25") == 0) {
    anc->anc_fps = ST_FPS_P25;
  } else if (strcmp(anc_fps, "p29") == 0) {
    anc->anc_fps = ST_FPS_P29_97;
  } else {
    err("%s, invalid anc fps %s\n", __func__, anc_fps);
    return -ST_JSON_NOT_VALID;
  }

  /* parse udp port */
  int start_port = json_object_get_int(st_json_object_object_get(anc_obj, "start_port"));
  if (start_port <= 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  anc->udp_port = start_port + idx;

  /* parse payload type */
  json_object* payload_type_object = st_json_object_object_get(anc_obj, "payload_type");
  int payload_type = ST_APP_PAYLOAD_TYPE_ANCILLARY;
  if (payload_type_object) {
    payload_type = json_object_get_int(payload_type_object);
    if (!st_json_is_valid_payload_type(payload_type)) {
      err("%s, invalid payload type %d\n", __func__, payload_type);
      return -ST_JSON_NOT_VALID;
    }
  }
  anc->payload_type = payload_type;

  /* parse anc url */
  const char* anc_url =
      json_object_get_string(st_json_object_object_get(anc_obj, "ancillary_url"));
  CHECK_STRING(anc_url);
  snprintf(anc->anc_url, sizeof(anc->anc_url), "%s", anc_url);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_tx_st22p(int idx, json_object* st22p_obj,
                                  st_json_tx_st22p_session_t* st22p) {
  if (st22p_obj == NULL || st22p == NULL) {
    err("%s, can not parse tx st22p session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse udp port */
  int start_port =
      json_object_get_int(st_json_object_object_get(st22p_obj, "start_port"));
  if (start_port <= 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  st22p->udp_port = start_port + idx;

  /* parse payload type */
  json_object* payload_type_object = st_json_object_object_get(st22p_obj, "payload_type");
  int payload_type = ST_APP_PAYLOAD_TYPE_ANCILLARY;
  if (payload_type_object) {
    payload_type = json_object_get_int(payload_type_object);
    if (!st_json_is_valid_payload_type(payload_type)) {
      err("%s, invalid payload type %d\n", __func__, payload_type);
      return -ST_JSON_NOT_VALID;
    }
  }
  st22p->payload_type = payload_type;

  /* parse width */
  int width = json_object_get_int(st_json_object_object_get(st22p_obj, "width"));
  if (width <= 0) {
    err("%s, invalid width %d\n", __func__, width);
    return -ST_JSON_NOT_VALID;
  }
  st22p->width = width;

  /* parse height */
  int height = json_object_get_int(st_json_object_object_get(st22p_obj, "height"));
  if (height <= 0) {
    err("%s, invalid height %d\n", __func__, height);
    return -ST_JSON_NOT_VALID;
  }
  st22p->height = height;

  /* parse fps */
  const char* fps = json_object_get_string(st_json_object_object_get(st22p_obj, "fps"));
  CHECK_STRING(fps);
  if (strcmp(fps, "p59") == 0) {
    st22p->fps = ST_FPS_P59_94;
  } else if (strcmp(fps, "p50") == 0) {
    st22p->fps = ST_FPS_P50;
  } else if (strcmp(fps, "p25") == 0) {
    st22p->fps = ST_FPS_P25;
  } else if (strcmp(fps, "p29") == 0) {
    st22p->fps = ST_FPS_P29_97;
  } else {
    err("%s, invalid anc fps %s\n", __func__, fps);
    return -ST_JSON_NOT_VALID;
  }

  /* parse pack_type */
  const char* pack_type =
      json_object_get_string(st_json_object_object_get(st22p_obj, "pack_type"));
  CHECK_STRING(pack_type);
  if (strcmp(pack_type, "codestream") == 0) {
    st22p->pack_type = ST22_PACK_CODESTREAM;
  } else if (strcmp(pack_type, "slice") == 0) {
    st22p->pack_type = ST22_PACK_SLICE;
  } else {
    err("%s, invalid pack_type %s\n", __func__, pack_type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse codec */
  const char* codec =
      json_object_get_string(st_json_object_object_get(st22p_obj, "codec"));
  CHECK_STRING(codec);
  if (strcmp(codec, "JPEG-XS") == 0) {
    st22p->codec = ST22_CODEC_JPEGXS;
  } else {
    err("%s, invalid codec %s\n", __func__, codec);
    return -ST_JSON_NOT_VALID;
  }

  /* parse device */
  const char* device =
      json_object_get_string(st_json_object_object_get(st22p_obj, "device"));
  CHECK_STRING(device);
  if (strcmp(device, "AUTO") == 0) {
    st22p->device = ST_PLUGIN_DEVICE_AUTO;
  } else if (strcmp(device, "CPU") == 0) {
    st22p->device = ST_PLUGIN_DEVICE_CPU;
  } else if (strcmp(device, "GPU") == 0) {
    st22p->device = ST_PLUGIN_DEVICE_GPU;
  } else if (strcmp(device, "FPGA") == 0) {
    st22p->device = ST_PLUGIN_DEVICE_FPGA;
  } else {
    err("%s, invalid plugin device type %s\n", __func__, device);
    return -ST_JSON_NOT_VALID;
  }

  /* parse quality */
  st22p->quality = ST22_QUALITY_MODE_SPEED;
  const char* quality =
      json_object_get_string(st_json_object_object_get(st22p_obj, "quality"));
  if (quality) {
    if (strcmp(quality, "quality") == 0) {
      st22p->quality = ST22_QUALITY_MODE_QUALITY;
    } else if (strcmp(quality, "speed") == 0) {
      st22p->quality = ST22_QUALITY_MODE_SPEED;
    } else {
      err("%s, invalid plugin quality type %s\n", __func__, quality);
      return -ST_JSON_NOT_VALID;
    }
  }

  /* parse input format */
  const char* format =
      json_object_get_string(st_json_object_object_get(st22p_obj, "input_format"));
  CHECK_STRING(format);
  if (strcmp(format, "YUV422PLANAR10LE") == 0) {
    st22p->format = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else if (strcmp(format, "ARGB") == 0) {
    st22p->format = ST_FRAME_FMT_ARGB;
  } else if (strcmp(format, "BGRA") == 0) {
    st22p->format = ST_FRAME_FMT_BGRA;
  } else if (strcmp(format, "V210") == 0) {
    st22p->format = ST_FRAME_FMT_V210;
  } else if (strcmp(format, "YUV422PLANAR8") == 0) {
    st22p->format = ST_FRAME_FMT_YUV422PLANAR8;
  } else if (strcmp(format, "YUV422PACKED8") == 0) {
    st22p->format = ST_FRAME_FMT_YUV422PACKED8;
  } else if (strcmp(format, "YUV422RFC4175PG2BE10") == 0) {
    st22p->format = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  } else if (strcmp(format, "RGB8") == 0) {
    st22p->format = ST_FRAME_FMT_RGB8;
  } else if (strcmp(format, "JPEGXS_CODESTREAM") == 0) {
    st22p->format = ST_FRAME_FMT_JPEGXS_CODESTREAM;
  } else {
    err("%s, invalid output format %s\n", __func__, format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse st22p url */
  const char* st22p_url =
      json_object_get_string(st_json_object_object_get(st22p_obj, "st22p_url"));
  CHECK_STRING(st22p_url);
  snprintf(st22p->st22p_url, sizeof(st22p->st22p_url), "%s", st22p_url);

  /* parse codec_thread_count option */
  st22p->codec_thread_count =
      json_object_get_int(st_json_object_object_get(st22p_obj, "codec_thread_count"));

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_video(int idx, json_object* video_obj,
                                  st_json_rx_video_session_t* video) {
  if (video_obj == NULL || video == NULL) {
    err("%s, can not parse rx video session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse video type */
  const char* type = json_object_get_string(st_json_object_object_get(video_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    video->type = ST20_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    video->type = ST20_TYPE_RTP_LEVEL;
  } else if (strcmp(type, "slice") == 0) {
    video->type = ST20_TYPE_SLICE_LEVEL;
  } else {
    err("%s, invalid video type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video pacing */
  const char* pacing =
      json_object_get_string(st_json_object_object_get(video_obj, "pacing"));
  CHECK_STRING(pacing);
  if (strcmp(pacing, "gap") == 0) {
    video->pacing = PACING_GAP;
  } else if (strcmp(pacing, "linear") == 0) {
    video->pacing = PACING_LINEAR;
  } else {
    err("%s, invalid video pacing %s\n", __func__, pacing);
    return -ST_JSON_NOT_VALID;
  }

  /* parse udp port */
  int start_port =
      json_object_get_int(st_json_object_object_get(video_obj, "start_port"));
  if (start_port <= 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  video->udp_port = start_port + idx;

  /* parse payload type */
  json_object* payload_type_object = st_json_object_object_get(video_obj, "payload_type");
  int payload_type = ST_APP_PAYLOAD_TYPE_VIDEO;
  if (payload_type_object) {
    payload_type = json_object_get_int(payload_type_object);
    if (!st_json_is_valid_payload_type(payload_type)) {
      err("%s, invalid payload type %d\n", __func__, payload_type);
      return -ST_JSON_NOT_VALID;
    }
  }
  video->payload_type = payload_type;

  /* parse tr offset */
  const char* tr_offset =
      json_object_get_string(st_json_object_object_get(video_obj, "tr_offset"));
  CHECK_STRING(tr_offset);
  if (strcmp(tr_offset, "default") == 0) {
    video->tr_offset = TR_OFFSET_DEFAULT;
  } else if (strcmp(pacing, "none") == 0) {
    video->tr_offset = TR_OFFSET_NONE;
  } else {
    err("%s, invalid video tr_offset %s\n", __func__, tr_offset);
    return -ST_JSON_NOT_VALID;
  }

  /* parse video format */
  const char* video_format =
      json_object_get_string(st_json_object_object_get(video_obj, "video_format"));
  CHECK_STRING(video_format);
  if (strcmp(video_format, "i1080p59") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_59FPS;
  } else if (strcmp(video_format, "i1080p50") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_50FPS;
  } else if (strcmp(video_format, "i1080i59") == 0) {
    video->video_format = VIDEO_FORMAT_1080I_59FPS;
  } else if (strcmp(video_format, "i1080i50") == 0) {
    video->video_format = VIDEO_FORMAT_1080I_50FPS;
  } else if (strcmp(video_format, "i1080p29") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_29FPS;
  } else if (strcmp(video_format, "i1080p25") == 0) {
    video->video_format = VIDEO_FORMAT_1080P_25FPS;
  } else if (strcmp(video_format, "i2160p59") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_59FPS;
  } else if (strcmp(video_format, "i2160p50") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_50FPS;
  } else if (strcmp(video_format, "i2160p29") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_29FPS;
  } else if (strcmp(video_format, "i2160p25") == 0) {
    video->video_format = VIDEO_FORMAT_2160P_25FPS;
  } else if (strcmp(video_format, "i720p59") == 0) {
    video->video_format = VIDEO_FORMAT_720P_59FPS;
  } else if (strcmp(video_format, "i720p50") == 0) {
    video->video_format = VIDEO_FORMAT_720P_50FPS;
  } else if (strcmp(video_format, "i720p29") == 0) {
    video->video_format = VIDEO_FORMAT_720P_29FPS;
  } else if (strcmp(video_format, "i720p25") == 0) {
    video->video_format = VIDEO_FORMAT_720P_25FPS;
  } else if (strcmp(video_format, "i4320p59") == 0) {
    video->video_format = VIDEO_FORMAT_4320P_59FPS;
  } else if (strcmp(video_format, "i4320p50") == 0) {
    video->video_format = VIDEO_FORMAT_4320P_50FPS;
  } else if (strcmp(video_format, "i4320p29") == 0) {
    video->video_format = VIDEO_FORMAT_4320P_29FPS;
  } else if (strcmp(video_format, "i4320p25") == 0) {
    video->video_format = VIDEO_FORMAT_4320P_25FPS;
  } else if (strcmp(video_format, "i480i59") == 0) {
    video->video_format = VIDEO_FORMAT_480I_59FPS;
  } else if (strcmp(video_format, "i576i50") == 0) {
    video->video_format = VIDEO_FORMAT_576I_50FPS;
  } else if (strcmp(video_format, "auto") == 0) {
    video->video_format = VIDEO_FORMAT_AUTO;
  } else {
    err("%s, invalid video format %s\n", __func__, video_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse pixel group format */
  const char* pg_format =
      json_object_get_string(st_json_object_object_get(video_obj, "pg_format"));
  CHECK_STRING(pg_format);
  if (strcmp(pg_format, "YUV_422_10bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_10BIT;
  } else if (strcmp(pg_format, "YUV_422_8bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_8BIT;
  } else if (strcmp(pg_format, "YUV_422_12bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_12BIT;
  } else if (strcmp(pg_format, "YUV_422_16bit") == 0) {
    video->pg_format = ST20_FMT_YUV_422_16BIT;
  } else if (strcmp(pg_format, "YUV_420_8bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_8BIT;
  } else if (strcmp(pg_format, "YUV_420_10bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_10BIT;
  } else if (strcmp(pg_format, "YUV_420_12bit") == 0) {
    video->pg_format = ST20_FMT_YUV_420_12BIT;
  } else if (strcmp(pg_format, "RGB_8bit") == 0) {
    video->pg_format = ST20_FMT_RGB_8BIT;
  } else if (strcmp(pg_format, "RGB_10bit") == 0) {
    video->pg_format = ST20_FMT_RGB_10BIT;
  } else if (strcmp(pg_format, "RGB_12bit") == 0) {
    video->pg_format = ST20_FMT_RGB_12BIT;
  } else if (strcmp(pg_format, "RGB_16bit") == 0) {
    video->pg_format = ST20_FMT_RGB_16BIT;
  } else {
    err("%s, invalid pixel group format %s\n", __func__, pg_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse user pixel group format */
  video->user_pg_format = USER_FMT_MAX;
  const char* user_pg_format =
      json_object_get_string(st_json_object_object_get(video_obj, "user_pg_format"));
  if (user_pg_format != NULL) {
    if (strcmp(user_pg_format, "YUV_422_8bit") == 0) {
      video->user_pg_format = USER_FMT_YUV_422_8BIT;
    } else {
      err("%s, invalid pixel group format %s\n", __func__, user_pg_format);
      return -ST_JSON_NOT_VALID;
    }
  }

  /* parse display option */
  video->display =
      json_object_get_boolean(st_json_object_object_get(video_obj, "display"));

  /* parse measure_latency option */
  video->measure_latency =
      json_object_get_boolean(st_json_object_object_get(video_obj, "measure_latency"));

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_audio(int idx, json_object* audio_obj,
                                  st_json_rx_audio_session_t* audio) {
  if (audio_obj == NULL || audio == NULL) {
    err("%s, can not parse rx audio session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse audio type */
  const char* type = json_object_get_string(st_json_object_object_get(audio_obj, "type"));
  CHECK_STRING(type);
  if (strcmp(type, "frame") == 0) {
    audio->type = ST30_TYPE_FRAME_LEVEL;
  } else if (strcmp(type, "rtp") == 0) {
    audio->type = ST30_TYPE_RTP_LEVEL;
  } else {
    err("%s, invalid audio type %s\n", __func__, type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio format */
  const char* audio_format =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_format"));
  CHECK_STRING(audio_format);
  if (strcmp(audio_format, "PCM8") == 0) {
    audio->audio_format = ST30_FMT_PCM8;
  } else if (strcmp(audio_format, "PCM16") == 0) {
    audio->audio_format = ST30_FMT_PCM16;
  } else if (strcmp(audio_format, "PCM24") == 0) {
    audio->audio_format = ST30_FMT_PCM24;
  } else if (strcmp(audio_format, "AM824") == 0) {
    audio->audio_format = ST31_FMT_AM824;
  } else {
    err("%s, invalid audio format %s\n", __func__, audio_format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio channel */
  json_object* audio_channel_array =
      st_json_object_object_get(audio_obj, "audio_channel");
  if (audio_channel_array == NULL ||
      json_object_get_type(audio_channel_array) != json_type_array) {
    err("%s, can not parse audio channel\n", __func__);
    return -ST_JSON_PARSE_FAIL;
  }
  for (int i = 0; i < json_object_array_length(audio_channel_array); ++i) {
    json_object* channel_obj = json_object_array_get_idx(audio_channel_array, i);
    const char* channel = json_object_get_string(channel_obj);
    CHECK_STRING(channel);
    if (strcmp(channel, "M") == 0) {
      audio->audio_channel += 1;
    } else if (strcmp(channel, "DM") == 0 || strcmp(channel, "ST") == 0 ||
               strcmp(channel, "LtRt") == 0 || strcmp(channel, "AES3") == 0) {
      audio->audio_channel += 2;
    } else if (strcmp(channel, "51") == 0) {
      audio->audio_channel += 6;
    } else if (strcmp(channel, "71") == 0) {
      audio->audio_channel += 8;
    } else if (strcmp(channel, "222") == 0) {
      audio->audio_channel += 24;
    } else if (strcmp(channel, "SGRP") == 0) {
      audio->audio_channel += 4;
    } else if (channel[0] == 'U' && channel[1] >= '0' && channel[1] <= '9' &&
               channel[2] >= '0' && channel[2] <= '9' && channel[3] == '\0') {
      int num_channel = (channel[1] - '0') * 10 + (channel[2] - '0');
      if (num_channel < 1 || num_channel > 64) {
        err("%s, audio undefined channel number out of range %s\n", __func__, channel);
        return -ST_JSON_NOT_VALID;
      }
      audio->audio_channel += num_channel;
    } else {
      err("%s, invalid audio channel %s\n", __func__, channel);
      return -ST_JSON_NOT_VALID;
    }
  }

  /* parse audio sampling */
  const char* audio_sampling =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_sampling"));
  CHECK_STRING(audio_sampling);
  if (strcmp(audio_sampling, "48kHz") == 0) {
    audio->audio_sampling = ST30_SAMPLING_48K;
  } else if (strcmp(audio_sampling, "96kHz") == 0) {
    audio->audio_sampling = ST30_SAMPLING_96K;
  } else if (strcmp(audio_sampling, "44.1kHz") == 0) {
    audio->audio_sampling = ST31_SAMPLING_44K;
  } else {
    err("%s, invalid audio sampling %s\n", __func__, audio_sampling);
    return -ST_JSON_NOT_VALID;
  }

  /* parse audio packet time */
  const char* audio_ptime =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_ptime"));
  // CHECK_STRING(audio_ptime);
  if (audio_ptime) {
    if (strcmp(audio_ptime, "1") == 0) {
      audio->audio_ptime = ST30_PTIME_1MS;
    } else if (strcmp(audio_ptime, "0.12") == 0) {
      audio->audio_ptime = ST30_PTIME_125US;
    } else if (strcmp(audio_ptime, "0.08") == 0) {
      audio->audio_ptime = ST30_PTIME_80US;
    } else if (strcmp(audio_ptime, "1.09") == 0) {
      audio->audio_ptime = ST31_PTIME_1_09MS;
    } else if (strcmp(audio_ptime, "0.14") == 0) {
      audio->audio_ptime = ST31_PTIME_0_14MS;
    } else if (strcmp(audio_ptime, "0.09") == 0) {
      audio->audio_ptime = ST31_PTIME_0_09MS;
    } else {
      err("%s, invalid audio ptime %s\n", __func__, audio_ptime);
      return -ST_JSON_NOT_VALID;
    }
  } else {
    audio->audio_ptime = ST30_PTIME_1MS;
  }

  /* parse udp port */
  int start_port =
      json_object_get_int(st_json_object_object_get(audio_obj, "start_port"));
  if (start_port <= 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  audio->udp_port = start_port + idx;

  /* parse payload type */
  json_object* payload_type_object = st_json_object_object_get(audio_obj, "payload_type");
  int payload_type = ST_APP_PAYLOAD_TYPE_AUDIO;
  if (payload_type_object) {
    payload_type = json_object_get_int(payload_type_object);
    if (!st_json_is_valid_payload_type(payload_type)) {
      err("%s, invalid payload type %d\n", __func__, payload_type);
      return -ST_JSON_NOT_VALID;
    }
  }
  audio->payload_type = payload_type;

  /* parse audio url */
  const char* audio_url =
      json_object_get_string(st_json_object_object_get(audio_obj, "audio_url"));
  CHECK_STRING(audio_url);
  snprintf(audio->audio_url, sizeof(audio->audio_url), "%s", audio_url);

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_anc(int idx, json_object* anc_obj,
                                st_json_rx_ancillary_session_t* anc) {
  if (anc_obj == NULL || anc == NULL) {
    err("%s, can not parse rx anc session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse udp port */
  int start_port = json_object_get_int(st_json_object_object_get(anc_obj, "start_port"));
  if (start_port <= 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  anc->udp_port = start_port + idx;

  /* parse payload type */
  json_object* payload_type_object = st_json_object_object_get(anc_obj, "payload_type");
  int payload_type = ST_APP_PAYLOAD_TYPE_ANCILLARY;
  if (payload_type_object) {
    payload_type = json_object_get_int(payload_type_object);
    if (!st_json_is_valid_payload_type(payload_type)) {
      err("%s, invalid payload type %d\n", __func__, payload_type);
      return -ST_JSON_NOT_VALID;
    }
  }
  anc->payload_type = payload_type;

  return ST_JSON_SUCCESS;
}

static int st_json_parse_rx_st22p(int idx, json_object* st22p_obj,
                                  st_json_rx_st22p_session_t* st22p) {
  if (st22p_obj == NULL || st22p == NULL) {
    err("%s, can not parse rx st22p session\n", __func__);
    return -ST_JSON_NULL;
  }

  /* parse udp port */
  int start_port =
      json_object_get_int(st_json_object_object_get(st22p_obj, "start_port"));
  if (start_port <= 0 || start_port > 65535) {
    err("%s, invalid start port %d\n", __func__, start_port);
    return -ST_JSON_NOT_VALID;
  }
  st22p->udp_port = start_port + idx;

  /* parse payload type */
  json_object* payload_type_object = st_json_object_object_get(st22p_obj, "payload_type");
  int payload_type = ST_APP_PAYLOAD_TYPE_ANCILLARY;
  if (payload_type_object) {
    payload_type = json_object_get_int(payload_type_object);
    if (!st_json_is_valid_payload_type(payload_type)) {
      err("%s, invalid payload type %d\n", __func__, payload_type);
      return -ST_JSON_NOT_VALID;
    }
  }
  st22p->payload_type = payload_type;

  /* parse width */
  int width = json_object_get_int(st_json_object_object_get(st22p_obj, "width"));
  if (width <= 0) {
    err("%s, invalid width %d\n", __func__, width);
    return -ST_JSON_NOT_VALID;
  }
  st22p->width = width;

  /* parse height */
  int height = json_object_get_int(st_json_object_object_get(st22p_obj, "height"));
  if (height <= 0) {
    err("%s, invalid height %d\n", __func__, height);
    return -ST_JSON_NOT_VALID;
  }
  st22p->height = height;

  /* parse fps */
  const char* fps = json_object_get_string(st_json_object_object_get(st22p_obj, "fps"));
  CHECK_STRING(fps);
  if (strcmp(fps, "p59") == 0) {
    st22p->fps = ST_FPS_P59_94;
  } else if (strcmp(fps, "p50") == 0) {
    st22p->fps = ST_FPS_P50;
  } else if (strcmp(fps, "p25") == 0) {
    st22p->fps = ST_FPS_P25;
  } else if (strcmp(fps, "p29") == 0) {
    st22p->fps = ST_FPS_P29_97;
  } else {
    err("%s, invalid anc fps %s\n", __func__, fps);
    return -ST_JSON_NOT_VALID;
  }

  /* parse codec */
  const char* codec =
      json_object_get_string(st_json_object_object_get(st22p_obj, "codec"));
  CHECK_STRING(codec);
  if (strcmp(codec, "JPEG-XS") == 0) {
    st22p->codec = ST22_CODEC_JPEGXS;
  } else {
    err("%s, invalid codec %s\n", __func__, codec);
    return -ST_JSON_NOT_VALID;
  }

  /* parse device */
  const char* device =
      json_object_get_string(st_json_object_object_get(st22p_obj, "device"));
  CHECK_STRING(device);
  if (strcmp(device, "AUTO") == 0) {
    st22p->device = ST_PLUGIN_DEVICE_AUTO;
  } else if (strcmp(device, "CPU") == 0) {
    st22p->device = ST_PLUGIN_DEVICE_CPU;
  } else if (strcmp(device, "GPU") == 0) {
    st22p->device = ST_PLUGIN_DEVICE_GPU;
  } else if (strcmp(device, "FPGA") == 0) {
    st22p->device = ST_PLUGIN_DEVICE_FPGA;
  } else {
    err("%s, invalid plugin device type %s\n", __func__, device);
    return -ST_JSON_NOT_VALID;
  }

  /* parse pack_type */
  const char* pack_type =
      json_object_get_string(st_json_object_object_get(st22p_obj, "pack_type"));
  CHECK_STRING(pack_type);
  if (strcmp(pack_type, "codestream") == 0) {
    st22p->pack_type = ST22_PACK_CODESTREAM;
  } else if (strcmp(pack_type, "slice") == 0) {
    st22p->pack_type = ST22_PACK_SLICE;
  } else {
    err("%s, invalid pack_type %s\n", __func__, pack_type);
    return -ST_JSON_NOT_VALID;
  }

  /* parse output format */
  const char* format =
      json_object_get_string(st_json_object_object_get(st22p_obj, "output_format"));
  CHECK_STRING(format);
  if (strcmp(format, "YUV422PLANAR10LE") == 0) {
    st22p->format = ST_FRAME_FMT_YUV422PLANAR10LE;
  } else if (strcmp(format, "ARGB") == 0) {
    st22p->format = ST_FRAME_FMT_ARGB;
  } else if (strcmp(format, "BGRA") == 0) {
    st22p->format = ST_FRAME_FMT_BGRA;
  } else if (strcmp(format, "V210") == 0) {
    st22p->format = ST_FRAME_FMT_V210;
  } else if (strcmp(format, "YUV422PLANAR8") == 0) {
    st22p->format = ST_FRAME_FMT_YUV422PLANAR8;
  } else if (strcmp(format, "YUV422PACKED8") == 0) {
    st22p->format = ST_FRAME_FMT_YUV422PACKED8;
  } else if (strcmp(format, "YUV422RFC4175PG2BE10") == 0) {
    st22p->format = ST_FRAME_FMT_YUV422RFC4175PG2BE10;
  } else if (strcmp(format, "RGB8") == 0) {
    st22p->format = ST_FRAME_FMT_RGB8;
  } else if (strcmp(format, "JPEGXS_CODESTREAM") == 0) {
    st22p->format = ST_FRAME_FMT_JPEGXS_CODESTREAM;
  } else {
    err("%s, invalid output format %s\n", __func__, format);
    return -ST_JSON_NOT_VALID;
  }

  /* parse display option */
  st22p->display =
      json_object_get_boolean(st_json_object_object_get(st22p_obj, "display"));

  /* parse measure_latency option */
  st22p->measure_latency =
      json_object_get_boolean(st_json_object_object_get(st22p_obj, "measure_latency"));

  /* parse codec_thread_count option */
  st22p->codec_thread_count =
      json_object_get_int(st_json_object_object_get(st22p_obj, "codec_thread_count"));

  return ST_JSON_SUCCESS;
}

int st_app_parse_json(st_json_context_t* ctx, const char* filename) {
  info("%s, using json-c version: %s\n", __func__, json_c_version());
  int ret = ST_JSON_SUCCESS;

  json_object* root_object = json_object_from_file(filename);
  if (root_object == NULL) {
    err("%s, can not parse json file %s, please check the format\n", __func__, filename);
    return -ST_JSON_PARSE_FAIL;
  }

  /* parse quota for system */
  json_object* sch_quota_object =
      st_json_object_object_get(root_object, "sch_session_quota");
  if (sch_quota_object != NULL) {
    int sch_quota = json_object_get_int(sch_quota_object);
    if (sch_quota <= 0) {
      err("%s, invalid quota number\n", __func__);
      ret = -ST_JSON_NOT_VALID;
      goto exit;
    }
    ctx->sch_quota = sch_quota;
  }

  /* parse interfaces for system */
  json_object* interfaces_array = st_json_object_object_get(root_object, "interfaces");
  if (interfaces_array == NULL ||
      json_object_get_type(interfaces_array) != json_type_array) {
    err("%s, can not parse interfaces\n", __func__);
    ret = -ST_JSON_PARSE_FAIL;
    goto exit;
  }
  int num_interfaces = json_object_array_length(interfaces_array);
  for (int i = 0; i < num_interfaces; ++i) {
    ret = st_json_parse_interfaces(json_object_array_get_idx(interfaces_array, i),
                                   &ctx->interfaces[i]);
    if (ret) goto exit;
  }
  ctx->num_interfaces = num_interfaces;

  /* parse tx sessions  */
  json_object* tx_group_array = st_json_object_object_get(root_object, "tx_sessions");
  if (tx_group_array != NULL && json_object_get_type(tx_group_array) == json_type_array) {
    int num_inf = 0;
    int num_video = 0;
    int num_audio = 0;
    int num_anc = 0;
    int num_st22p = 0;

    for (int i = 0; i < json_object_array_length(tx_group_array); ++i) {
      json_object* tx_group = json_object_array_get_idx(tx_group_array, i);
      if (tx_group == NULL) {
        err("%s, can not parse tx session group\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse destination ip */
      json_object* dip_p = NULL;
      json_object* dip_r = NULL;
      json_object* dip_array = st_json_object_object_get(tx_group, "dip");
      if (dip_array != NULL && json_object_get_type(dip_array) == json_type_array) {
        int len = json_object_array_length(dip_array);
        if (len < 1 || len > ST_PORT_MAX) {
          err("%s, wrong dip number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        dip_p = json_object_array_get_idx(dip_array, 0);
        if (len == 2) {
          dip_r = json_object_array_get_idx(dip_array, 1);
        }
        num_inf = len;
      } else {
        err("%s, can not parse dip_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse interface */
      int inf_p, inf_r = 0;
      json_object* interface_array = st_json_object_object_get(tx_group, "interface");
      if (interface_array != NULL &&
          json_object_get_type(interface_array) == json_type_array) {
        int len = json_object_array_length(interface_array);
        if (len != num_inf) {
          err("%s, wrong interface number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        inf_p = json_object_get_int(json_object_array_get_idx(interface_array, 0));
        if (inf_p < 0 || inf_p > num_interfaces) {
          err("%s, wrong interface index\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        if (len == 2) {
          inf_r = json_object_get_int(json_object_array_get_idx(interface_array, 1));
          if (inf_r < 0 || inf_r > num_interfaces) {
            err("%s, wrong interface index\n", __func__);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
        }
      } else {
        err("%s, can not parse interface_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse tx video sessions */
      json_object* video_array = st_json_object_object_get(tx_group, "video");
      if (video_array != NULL && json_object_get_type(video_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(video_array); ++j) {
          json_object* video_session = json_object_array_get_idx(video_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(video_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(dip_p),
                      ctx->tx_video[num_video].dip[0]);
            ctx->tx_video[num_video].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(dip_r),
                        ctx->tx_video[num_video].dip[1]);
              ctx->tx_video[num_video].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_video[num_video].num_inf = num_inf;
            ret = st_json_parse_tx_video(k, video_session, &ctx->tx_video[num_video]);
            if (ret) goto exit;
            num_video++;
          }
        }
      }

      /* parse tx audio sessions */
      json_object* audio_array = st_json_object_object_get(tx_group, "audio");
      if (audio_array != NULL && json_object_get_type(audio_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(audio_array); ++j) {
          json_object* audio_session = json_object_array_get_idx(audio_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(audio_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(dip_p),
                      ctx->tx_audio[num_audio].dip[0]);
            ctx->tx_audio[num_audio].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(dip_r),
                        ctx->tx_audio[num_audio].dip[1]);
              ctx->tx_audio[num_audio].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_audio[num_audio].num_inf = num_inf;
            ret = st_json_parse_tx_audio(k, audio_session, &ctx->tx_audio[num_audio]);
            if (ret) goto exit;
            num_audio++;
          }
        }
      }

      /* parse tx ancillary sessions */
      json_object* anc_array = st_json_object_object_get(tx_group, "ancillary");
      if (anc_array != NULL && json_object_get_type(anc_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(anc_array); ++j) {
          json_object* anc_session = json_object_array_get_idx(anc_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(anc_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(dip_p),
                      ctx->tx_anc[num_anc].dip[0]);
            ctx->tx_anc[num_anc].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(dip_r),
                        ctx->tx_anc[num_anc].dip[1]);
              ctx->tx_anc[num_anc].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_anc[num_anc].num_inf = num_inf;
            ret = st_json_parse_tx_anc(k, anc_session, &ctx->tx_anc[num_anc]);
            if (ret) goto exit;
            num_anc++;
          }
        }
      }

      /* parse rx st22p sessions */
      json_object* st22p_array = st_json_object_object_get(tx_group, "st22p");
      if (st22p_array != NULL && json_object_get_type(st22p_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(st22p_array); ++j) {
          json_object* st22p_session = json_object_array_get_idx(st22p_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(st22p_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(dip_p),
                      ctx->tx_st22p[num_st22p].dip[0]);
            ctx->tx_st22p[num_st22p].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(dip_r),
                        ctx->tx_st22p[num_st22p].dip[1]);
              ctx->tx_st22p[num_st22p].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->tx_st22p[num_st22p].num_inf = num_inf;
            ret = st_json_parse_tx_st22p(k, st22p_session, &ctx->tx_st22p[num_st22p]);
            if (ret) goto exit;
            num_st22p++;
          }
        }
      }
    }

    ctx->tx_video_session_cnt = num_video;
    ctx->tx_audio_session_cnt = num_audio;
    ctx->tx_anc_session_cnt = num_anc;
    ctx->tx_st22p_session_cnt = num_st22p;
  }

  /* parse rx sessions */
  json_object* rx_group_array = st_json_object_object_get(root_object, "rx_sessions");
  if (rx_group_array != NULL && json_object_get_type(rx_group_array) == json_type_array) {
    int num_inf = 0;
    int num_video = 0;
    int num_audio = 0;
    int num_anc = 0;
    int num_st22p = 0;

    for (int i = 0; i < json_object_array_length(rx_group_array); ++i) {
      json_object* rx_group = json_object_array_get_idx(rx_group_array, i);
      if (rx_group == NULL) {
        err("%s, can not parse rx session group\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse receiving ip */
      json_object* ip_p = NULL;
      json_object* ip_r = NULL;
      json_object* ip_array = st_json_object_object_get(rx_group, "ip");
      if (ip_array != NULL && json_object_get_type(ip_array) == json_type_array) {
        int len = json_object_array_length(ip_array);
        if (len < 1 || len > ST_PORT_MAX) {
          err("%s, wrong dip number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        ip_p = json_object_array_get_idx(ip_array, 0);
        if (len == 2) {
          ip_r = json_object_array_get_idx(ip_array, 1);
        }
        num_inf = len;
      } else {
        err("%s, can not parse dip_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse interface */
      int inf_p, inf_r = 0;
      json_object* interface_array = st_json_object_object_get(rx_group, "interface");
      if (interface_array != NULL &&
          json_object_get_type(interface_array) == json_type_array) {
        int len = json_object_array_length(interface_array);
        if (len != num_inf) {
          err("%s, wrong interface number\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        inf_p = json_object_get_int(json_object_array_get_idx(interface_array, 0));
        if (inf_p < 0 || inf_p > num_interfaces) {
          err("%s, wrong interface index\n", __func__);
          ret = -ST_JSON_NOT_VALID;
          goto exit;
        }
        if (len == 2) {
          inf_r = json_object_get_int(json_object_array_get_idx(interface_array, 1));
          if (inf_r < 0 || inf_r > num_interfaces) {
            err("%s, wrong interface index\n", __func__);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
        }
      } else {
        err("%s, can not parse interface_array\n", __func__);
        ret = -ST_JSON_PARSE_FAIL;
        goto exit;
      }

      /* parse rx video sessions */
      json_object* video_array = st_json_object_object_get(rx_group, "video");
      if (video_array != NULL && json_object_get_type(video_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(video_array); ++j) {
          json_object* video_session = json_object_array_get_idx(video_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(video_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(ip_p),
                      ctx->rx_video[num_video].ip[0]);
            ctx->rx_video[num_video].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(ip_r),
                        ctx->rx_video[num_video].ip[1]);
              ctx->rx_video[num_video].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_video[num_video].num_inf = num_inf;
            ret = st_json_parse_rx_video(k, video_session, &ctx->rx_video[num_video]);
            if (ret) goto exit;
            num_video++;
          }
        }
      }

      /* parse rx audio sessions */
      json_object* audio_array = st_json_object_object_get(rx_group, "audio");
      if (audio_array != NULL && json_object_get_type(audio_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(audio_array); ++j) {
          json_object* audio_session = json_object_array_get_idx(audio_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(audio_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(ip_p),
                      ctx->rx_audio[num_audio].ip[0]);
            ctx->rx_audio[num_audio].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(ip_r),
                        ctx->rx_audio[num_audio].ip[1]);
              ctx->rx_audio[num_audio].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_audio[num_audio].num_inf = num_inf;
            ret = st_json_parse_rx_audio(k, audio_session, &ctx->rx_audio[num_audio]);
            if (ret) goto exit;
            num_audio++;
          }
        }
      }

      /* parse rx ancillary sessions */
      json_object* anc_array = st_json_object_object_get(rx_group, "ancillary");
      if (anc_array != NULL && json_object_get_type(anc_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(anc_array); ++j) {
          json_object* anc_session = json_object_array_get_idx(anc_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(anc_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(ip_p), ctx->rx_anc[num_anc].ip[0]);
            ctx->rx_anc[num_anc].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(ip_r),
                        ctx->rx_anc[num_anc].ip[1]);
              ctx->rx_anc[num_anc].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_anc[num_anc].num_inf = num_inf;
            ret = st_json_parse_rx_anc(k, anc_session, &ctx->rx_anc[num_anc]);
            if (ret) goto exit;
            num_anc++;
          }
        }
      }

      /* parse rx st22p sessions */
      json_object* st22p_array = st_json_object_object_get(rx_group, "st22p");
      if (st22p_array != NULL && json_object_get_type(st22p_array) == json_type_array) {
        for (int j = 0; j < json_object_array_length(st22p_array); ++j) {
          json_object* st22p_session = json_object_array_get_idx(st22p_array, j);
          int replicas =
              json_object_get_int(st_json_object_object_get(st22p_session, "replicas"));
          if (replicas < 0) {
            err("%s, invalid replicas number: %d\n", __func__, replicas);
            ret = -ST_JSON_NOT_VALID;
            goto exit;
          }
          for (int k = 0; k < replicas; ++k) {
            inet_pton(AF_INET, json_object_get_string(ip_p),
                      ctx->rx_st22p[num_st22p].ip[0]);
            ctx->rx_st22p[num_st22p].inf[0] = &ctx->interfaces[inf_p];
            if (num_inf == 2) {
              inet_pton(AF_INET, json_object_get_string(ip_r),
                        ctx->rx_st22p[num_st22p].ip[1]);
              ctx->rx_st22p[num_st22p].inf[1] = &ctx->interfaces[inf_r];
            }
            ctx->rx_st22p[num_st22p].num_inf = num_inf;
            ret = st_json_parse_rx_st22p(k, st22p_session, &ctx->rx_st22p[num_st22p]);
            if (ret) goto exit;
            num_st22p++;
          }
        }
      }
    }

    ctx->rx_video_session_cnt = num_video;
    ctx->rx_audio_session_cnt = num_audio;
    ctx->rx_anc_session_cnt = num_anc;
    ctx->rx_st22p_session_cnt = num_st22p;
  }

exit:
  json_object_put(root_object);
  return ret;
}

enum st_fps st_app_get_fps(enum video_format fmt) {
  switch (fmt) {
    case VIDEO_FORMAT_480I_59FPS:
    case VIDEO_FORMAT_720P_59FPS:
    case VIDEO_FORMAT_1080P_59FPS:
    case VIDEO_FORMAT_1080I_59FPS:
    case VIDEO_FORMAT_2160P_59FPS:
    case VIDEO_FORMAT_4320P_59FPS:
      return ST_FPS_P59_94;
    case VIDEO_FORMAT_720P_50FPS:
    case VIDEO_FORMAT_576I_50FPS:
    case VIDEO_FORMAT_1080P_50FPS:
    case VIDEO_FORMAT_1080I_50FPS:
    case VIDEO_FORMAT_2160P_50FPS:
    case VIDEO_FORMAT_4320P_50FPS:
      return ST_FPS_P50;
    case VIDEO_FORMAT_720P_25FPS:
    case VIDEO_FORMAT_1080P_25FPS:
    case VIDEO_FORMAT_2160P_25FPS:
    case VIDEO_FORMAT_4320P_25FPS:
      return ST_FPS_P25;
    case VIDEO_FORMAT_720P_29FPS:
    case VIDEO_FORMAT_1080P_29FPS:
    case VIDEO_FORMAT_2160P_29FPS:
    case VIDEO_FORMAT_4320P_29FPS:
      return ST_FPS_P29_97;
    default: {
      err("%s, invalid video fmt %d\n", __func__, fmt);
      return ST_FPS_P59_94;
    }
  }
}
int st_app_get_width(enum video_format fmt) {
  switch (fmt) {
    case VIDEO_FORMAT_720P_59FPS:
    case VIDEO_FORMAT_720P_50FPS:
    case VIDEO_FORMAT_720P_29FPS:
    case VIDEO_FORMAT_720P_25FPS:
      return 1280;
    case VIDEO_FORMAT_1080P_59FPS:
    case VIDEO_FORMAT_1080P_50FPS:
    case VIDEO_FORMAT_1080P_29FPS:
    case VIDEO_FORMAT_1080I_59FPS:
    case VIDEO_FORMAT_1080I_50FPS:
    case VIDEO_FORMAT_1080P_25FPS:
      return 1920;
    case VIDEO_FORMAT_2160P_59FPS:
    case VIDEO_FORMAT_2160P_50FPS:
    case VIDEO_FORMAT_2160P_29FPS:
    case VIDEO_FORMAT_2160P_25FPS:
      return 3840;
    case VIDEO_FORMAT_4320P_59FPS:
    case VIDEO_FORMAT_4320P_50FPS:
    case VIDEO_FORMAT_4320P_29FPS:
    case VIDEO_FORMAT_4320P_25FPS:
      return 7680;
    case VIDEO_FORMAT_480I_59FPS:
    case VIDEO_FORMAT_576I_50FPS:
      return 720;
    default: {
      err("%s, invalid video fmt %d\n", __func__, fmt);
      return 1920;
    }
  }
}
int st_app_get_height(enum video_format fmt) {
  switch (fmt) {
    case VIDEO_FORMAT_480I_59FPS:
      return 480;
    case VIDEO_FORMAT_576I_50FPS:
      return 576;
    case VIDEO_FORMAT_720P_59FPS:
    case VIDEO_FORMAT_720P_50FPS:
    case VIDEO_FORMAT_720P_29FPS:
    case VIDEO_FORMAT_720P_25FPS:
      return 720;
    case VIDEO_FORMAT_1080P_59FPS:
    case VIDEO_FORMAT_1080P_50FPS:
    case VIDEO_FORMAT_1080P_29FPS:
    case VIDEO_FORMAT_1080I_59FPS:
    case VIDEO_FORMAT_1080I_50FPS:
    case VIDEO_FORMAT_1080P_25FPS:
      return 1080;
    case VIDEO_FORMAT_2160P_59FPS:
    case VIDEO_FORMAT_2160P_50FPS:
    case VIDEO_FORMAT_2160P_29FPS:
    case VIDEO_FORMAT_2160P_25FPS:
      return 2160;
    case VIDEO_FORMAT_4320P_59FPS:
    case VIDEO_FORMAT_4320P_50FPS:
    case VIDEO_FORMAT_4320P_29FPS:
    case VIDEO_FORMAT_4320P_25FPS:
      return 4320;
    default: {
      err("%s, invalid video fmt %d\n", __func__, fmt);
      return 1080;
    }
  }
}
bool st_app_get_interlaced(enum video_format fmt) {
  switch (fmt) {
    case VIDEO_FORMAT_480I_59FPS:
    case VIDEO_FORMAT_576I_50FPS:
    case VIDEO_FORMAT_1080I_59FPS:
    case VIDEO_FORMAT_1080I_50FPS:
      return true;
    default: {
      return false;
    }
  }
}