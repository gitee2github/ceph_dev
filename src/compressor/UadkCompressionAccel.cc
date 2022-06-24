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

#include <atomic>
#include <pthread.h>
#include "unistd.h"
#include "common/debug.h"
#include "UadkCompressionAccel.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_compressor
#undef dout_prefix
#define dout_prefix _prefix(_dout)

#define NEED_MORE_OUT_BUFFER  5
#define PROCESS_NOT_FINISH    6
#define UADK_MIN_BUFFER       (32*1024)
#define UADK_MAX_BUFFER       (8*1024*1024)
#define UADK_DEFAULT_WIN_SIZE 15
#define UADK_PF_SIZE          1024

static ostream&
_prefix(std::ostream* _dout)
{
  return *_dout << "Compression--UADK: ";
}

static std::atomic<bool> init_called = { false };
static std::atomic<size_t> uadk_compressor_thread_num = 0;
std::mutex uadk_lock;

struct UadkEngine {
  struct wd_ctx_config ctx_cfg;
  struct wd_sched *sched;
  int numa_id;
} engine;


static int lib_poll_func(__u32 pos, __u32 expect, __u32 *count)
{
  int ret = wd_comp_poll_ctx(pos, expect, count);
  if (ret < 0)
    return ret;
  return 0;
}

static unsigned int get_uadk_ctx_num()
{
  char *env_set = getenv("WD_SYNC_CTX_NUM");
  if (env_set == nullptr) {
    return 2;
  } else {
    int num = stoi(string(env_set));
    if (num > UADK_PF_SIZE) {
      num = UADK_PF_SIZE;
    } else {
      num = std::max(num, 2);
    }
    return (unsigned int)num;
  }
}

static int uadk_init()
{
  if (init_called) {
    dout(10) << __func__ << ": UADK already init." << dendl;
    return 0;
  }

  int ret = 0;
  engine.sched = wd_sched_rr_alloc(SCHED_POLICY_RR, 2, 4, lib_poll_func);

  if (engine.sched == nullptr) {
    derr << __func__ << ": wd_sched_rr_alloc fail" << dendl;
    return -ENOMEM;
  }
  engine.sched->name = "sched_rr";

  struct uacce_dev *uadk_dev = wd_get_accel_dev("zlib");
  if (uadk_dev == nullptr) {
    derr << __func__ << ": cannot get uadk device " << dendl;
    ret = -ECANCELED;
    return ret;
  }
  engine.numa_id = uadk_dev->numa_id;
  unsigned int CMPRS_CTX_NUM = get_uadk_ctx_num();
  engine.ctx_cfg.ctx_num = CMPRS_CTX_NUM;
  engine.ctx_cfg.ctxs = new wd_ctx[CMPRS_CTX_NUM];

  unsigned int i;

  /******** request ctxs (compress ctx num + decompress ctx num) ********/
  for (i = 0; i != CMPRS_CTX_NUM; ++i) {
    engine.ctx_cfg.ctxs[i].ctx = wd_request_ctx(uadk_dev);
    if (!engine.ctx_cfg.ctxs[i].ctx) {
      derr << __func__ << ": UADK ctx ERROR !" << dendl;
      ret = -ECANCELED;
      goto out_fill;
    }
  }

  struct sched_params param;
  /******** create sched instance for compress ctx ********/
  for(unsigned int m = 0; m != CMPRS_CTX_NUM / 2; ++m) {
    engine.ctx_cfg.ctxs[m].op_type = WD_DIR_COMPRESS;
    engine.ctx_cfg.ctxs[m].ctx_mode = CTX_MODE_SYNC;
  }
  param.numa_id = engine.numa_id;
  param.type = WD_DIR_COMPRESS;
  param.mode = CTX_MODE_SYNC;
  param.begin = 0;
  param.end = CMPRS_CTX_NUM / 2 - 1;

  ret = wd_sched_rr_instance((const struct wd_sched *)engine.sched, &param);
  if (ret < 0) {
    derr << __func__ << ": Fail to fill compress sched region."
	 << "(" << ret << ")" << dendl;
    goto out_fill;
  }

  /******** create sched instance for decompress ctx ********/
  for(unsigned int m = CMPRS_CTX_NUM / 2; m != CMPRS_CTX_NUM; ++m) {
    engine.ctx_cfg.ctxs[m].op_type = WD_DIR_DECOMPRESS;
    engine.ctx_cfg.ctxs[m].ctx_mode = CTX_MODE_SYNC;
  }
  param.type = WD_DIR_DECOMPRESS;
  param.mode = CTX_MODE_SYNC;
  param.begin = CMPRS_CTX_NUM / 2;
  param.end = CMPRS_CTX_NUM - 1;
  ret = wd_sched_rr_instance((const struct wd_sched *)engine.sched, &param);
  if (ret < 0) {
    derr << __func__ << ": Fail to fill decompress sched region."
	 << "(" << ret << ")" << dendl;
    goto out_fill;
  }

  ret = wd_comp_init(&engine.ctx_cfg, engine.sched);
  if (ret != 0) {
    derr << __func__ << ": fail to init UADK !"
	 << "(" << ret << ")" << dendl;
    goto out_fill;
  }

  free(uadk_dev);
  uadk_dev = nullptr;
  init_called = true;
  return 0;

out_fill:
  for (unsigned int j = 0; j != i; ++j)
    wd_release_ctx(engine.ctx_cfg.ctxs[j].ctx);

  delete[] engine.ctx_cfg.ctxs;
  wd_sched_rr_release(engine.sched);
  engine.sched = nullptr;
  free(uadk_dev);
  uadk_dev = nullptr;
  return ret;
}

bool UadkCompressionAccel::init()
{
  ++uadk_compressor_thread_num;

  if (init_called) {
    dout(10) << __func__ << ": UADK already init." << dendl;
    return true;
  }

  uadk_lock.lock();
  int ret = uadk_init();
  uadk_lock.unlock();

  if (ret != 0) {
    derr << __func__ << ": fail to init uadk.(ret=" << ret << ")" << dendl;
    --uadk_compressor_thread_num;
    return false;
  }

  return true;
}

handle_t UadkCompressionAccel::create_comp_session()
{
  struct wd_comp_sess_setup setup;
  struct sched_params ss_param = {0};

  setup.op_type = WD_DIR_COMPRESS;
  setup.alg_type = WD_ZLIB;
  setup.comp_lv = WD_COMP_L1;
  setup.win_sz = WD_COMP_WS_8K;

  ss_param.type = setup.op_type;
  ss_param.numa_id = engine.numa_id;
  setup.sched_param = &ss_param;
  handle_t h_comp_sess = wd_comp_alloc_sess(&setup);
  return h_comp_sess;
}

void UadkCompressionAccel::free_comp_session(handle_t h_comp_sess)
{
  if (h_comp_sess) {
    wd_comp_free_sess(h_comp_sess);
    h_comp_sess = 0;
  }
}

handle_t UadkCompressionAccel::create_decomp_session()
{
  struct wd_comp_sess_setup de_setup;
  struct sched_params ss_de_param = {0};

  de_setup.op_type = WD_DIR_DECOMPRESS;
  de_setup.alg_type = WD_ZLIB;
  de_setup.comp_lv = WD_COMP_L1;
  de_setup.win_sz = WD_COMP_WS_32K;

  ss_de_param.type = de_setup.op_type;
  ss_de_param.numa_id = engine.numa_id;
  de_setup.sched_param = &ss_de_param;
  handle_t h_decomp_sess = wd_comp_alloc_sess(&de_setup);
  return h_decomp_sess;
}

void UadkCompressionAccel::free_decomp_session(handle_t h_decomp_sess)
{
  if (h_decomp_sess) {
    wd_comp_free_sess(h_decomp_sess);
    h_decomp_sess = 0;
  }
}

int UadkCompressionAccel::uadk_do_compress(handle_t h_sess, const unsigned char* in, unsigned int &inlen,
		                           unsigned char *out, unsigned int &outlen, bool last_packet)
{
  struct wd_comp_req req;

  req.op_type = WD_DIR_COMPRESS;
  req.src = const_cast<unsigned char*>(in);
  req.src_len = inlen;
  req.dst = out;
  req.dst_len = outlen;
  req.data_fmt = WD_FLAT_BUF;
  req.cb = nullptr;
  if (last_packet)
    req.last = 1;
  else
    req.last = 0;

  int ret = wd_do_comp_strm(h_sess, &req);
  if (ret == 0) {
    if (inlen > req.src_len) {
      inlen = req.src_len;
      outlen = req.dst_len;
      return NEED_MORE_OUT_BUFFER;
    } else {
      outlen = req.dst_len;
      return ret;
    }
  }

  return ret;
}

int UadkCompressionAccel::compress(const bufferlist &in, bufferlist &out, boost::optional<int32_t> &compressor_message)
{
  handle_t h_comp_sess = create_comp_session();
  unsigned int begin = 1;
  unsigned int out_len = 0;
  compressor_message = UADK_DEFAULT_WIN_SIZE;
  for (ceph::bufferlist::buffers_t::const_iterator i = in.buffers().begin(); i != in.buffers().end();) {
    const unsigned char* c_in = (unsigned char*) (*i).c_str();
    unsigned int len = (*i).length();
    unsigned int in_len = len;
    int ret = 0;
    ++i;

    bool last_ptr = (i == in.buffers().end()) ? true : false;

    do {
      if (len * 2 < UADK_MIN_BUFFER) {
        out_len = UADK_MIN_BUFFER;
      } else {
        out_len = std::min<size_t>(UADK_MAX_BUFFER, len * 2);
      }
      bufferptr ptr = buffer::create_small_page_aligned(out_len);
      unsigned char* c_out = (unsigned char*)ptr.c_str() + begin;
      in_len = std::min<size_t>(UADK_MAX_BUFFER, in_len);
      if (begin) {
        ptr.c_str()[0] = 0;
	out_len -= begin;
      }

      bool last_packet = last_ptr && (in_len == len);
      memset(c_out, 0, out_len);
      ret = uadk_do_compress(h_comp_sess, c_in, in_len, c_out, out_len, last_packet);
      if (ret < 0) {
        derr << __func__ << ": UADK deflation failed."
	     << "(" << ret << ")" << dendl;
	free_comp_session(h_comp_sess);
	return ret;
      }

      c_in += in_len;
      in_len = len - in_len;
      len = in_len;

      out.append(ptr, 0, out_len + begin);
      begin = 0;
    } while (ret == NEED_MORE_OUT_BUFFER || len > 0);
  }

  free_comp_session(h_comp_sess);
  return 0;
}

int UadkCompressionAccel::uadk_do_decompress(handle_t h_sess, const unsigned char *in, unsigned int &inlen,
		                             unsigned char *out, unsigned int &outlen)
{
  struct wd_comp_req req;

  req.op_type = WD_DIR_DECOMPRESS;
  req.data_fmt = WD_FLAT_BUF;
  req.cb = nullptr;

  req.src = const_cast<unsigned char*>(in);
  req.src_len = inlen;
  req.dst = out;
  req.dst_len = outlen;

  int ret = wd_do_comp_strm(h_sess, &req);

  if (ret == 0) {
    if (inlen > req.src_len) {
      inlen = req.src_len;
      outlen = req.dst_len;
      return NEED_MORE_OUT_BUFFER;
    } else if (req.status != WD_STREAM_END) {
      inlen = req.src_len;
      outlen = req.dst_len;
      return PROCESS_NOT_FINISH;
    } else {
      outlen = req.dst_len;
      return ret;
    }
  }

  return ret;
}

unsigned int cal_approx_ratio(unsigned int n, unsigned m)
{
  unsigned int x = 0;
  m /= n;
  while (m != 0) {
    m >>= 1;
    ++x;
  }
  return x + 1;
}

int UadkCompressionAccel::decompress(bufferlist::const_iterator &p, size_t compressed_len, bufferlist &dst,
		                     boost::optional<int32_t> compressor_message)
{
  handle_t h_decomp_sess = create_decomp_session();
  unsigned int begin = 1;
  unsigned int out_len = 0;
  unsigned int probe_ratio = 2;
  bufferptr ptr;
  size_t remaining = std::min<size_t>(p.get_remaining(), compressed_len);

  while (remaining) {
    const char *c_in;
    unsigned int len = p.get_ptr_and_advance(remaining, &c_in) - begin;
    unsigned int in_len = len;
    unsigned char *in = (unsigned char *)c_in + begin;
    int ret = 0;

    remaining -= (in_len + begin);
    begin = 0;

    do {
      if ((len << probe_ratio) < UADK_MIN_BUFFER) {
        out_len = UADK_MIN_BUFFER;
      } else {
        out_len = std::min<size_t>(UADK_MAX_BUFFER, (len << probe_ratio));
      }
      ptr = buffer::create_small_page_aligned(out_len);
      unsigned char* out = (unsigned char*)ptr.c_str();
      in_len = std::min<size_t>(UADK_MAX_BUFFER, in_len);
      memset(out, 0, out_len);
      ret = uadk_do_decompress(h_decomp_sess, in, in_len, out, out_len);
      if (ret < 0) {
        derr << __func__ << ": UADK inflation failed.(ret=" << ret << ")" << dendl;
	free_decomp_session(h_decomp_sess);
	return ret;
      }

     probe_ratio = cal_approx_ratio(in_len, out_len);
     in += in_len;
     in_len = len - in_len;
     len = in_len;
     dst.append(ptr, 0, out_len);
    } while (ret == NEED_MORE_OUT_BUFFER || (ret == PROCESS_NOT_FINISH && remaining ==0) || len > 0);
  }

  free_decomp_session(h_decomp_sess);
  return 0;
}

int UadkCompressionAccel::decompress(const bufferlist &in, bufferlist &out, boost::optional<int32_t> compressor_message)
{
  auto i = in.begin();
  return decompress(i, in.length(), out, compressor_message);
}

void UadkCompressionAccel::destory()
{
  if (!init_called) {
    return;
  }

  if (--uadk_compressor_thread_num != 0) {
    dout(10) << __func__ << ": " << uadk_compressor_thread_num << " threads need uadk zip" << dendl;
    return;
  }

  wd_comp_uninit();

  for (unsigned int i = 0; i < engine.ctx_cfg.ctx_num; i++) {
    wd_release_ctx(engine.ctx_cfg.ctxs[i].ctx);
  }
  delete[] engine.ctx_cfg.ctxs;
  wd_sched_rr_release(engine.sched);
  engine.sched = nullptr;
  init_called = false;
}
