/*
 * Ceph - scalable distributed file system
 *
 * Copyright (c) 2022 Huawei Technologies Co., Ltd All rights reserved.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_UADKCOMPRESSIONACCEL_H
#define CEPH_UADKCOMPRESSIONACCEL_H

#include <boost/optional.hpp>
#include "include/buffer.h"

extern "C" {
#include <uadk/wd_comp.h>
#include <uadk/wd.h>
#include <uadk/wd_sched.h>
}

class UadkCompressionAccel {
  public:
      UadkCompressionAccel() {  }
      ~UadkCompressionAccel() { destory(); }

      bool init();
      void destory();

      int compress(const bufferlist &in, bufferlist &out, boost::optional<int32_t> &compressor_message);
      int decompress(const bufferlist &in, bufferlist &out, boost::optional<int32_t> compressor_message);
      int decompress(bufferlist::const_iterator &p, size_t compressed_len, bufferlist &dst, boost::optional<int32_t> compressor_message);
  private:
      int uadk_do_compress(handle_t h_sess, const unsigned char *in, unsigned int &inlen, unsigned char *out, unsigned int &outlen, bool last_packet);
      int uadk_do_decompress(handle_t h_sess, const unsigned char *in, unsigned int &inlen, unsigned char *out, unsigned int &outlen);
      handle_t create_comp_session();
      handle_t create_decomp_session();
      void free_comp_session(handle_t h_sess);
      void free_decomp_session(handle_t h_sess);
};

#endif