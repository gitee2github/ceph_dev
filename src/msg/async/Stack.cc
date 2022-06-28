// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 XSky <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <mutex>

#include "include/compat.h"
#include "common/Cond.h"
#include "common/errno.h"
#include "PosixStack.h"
#include "AsyncConnection.h"

#ifdef HAVE_RDMA
#include "rdma/RDMAStack.h"
#endif
#ifdef HAVE_DPDK
#include "dpdk/DPDKStack.h"
#endif

#include "common/dout.h"
#include "include/ceph_assert.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << "stack "

std::function<void ()> NetworkStack::add_thread(unsigned worker_id)
{
  Worker *w = workers[worker_id];
  return [this, w]() {
      char tp_name[16];
      sprintf(tp_name, "msgr-worker-%u", w->id);
      ceph_pthread_setname(pthread_self(), tp_name);
      const unsigned EventMaxWaitUs = 30000000;
      w->center.set_owner();
      ldout(cct, 10) << __func__ << " starting" << dendl;
      w->initialize();
      w->init_done();
      while (!w->done) {
        ldout(cct, 30) << __func__ << " calling event process" << dendl;

        ceph::timespan dur;
        int r = w->center.process_events(EventMaxWaitUs, &dur);
        if (r < 0) {
          ldout(cct, 20) << __func__ << " process events failed: "
                         << cpp_strerror(errno) << dendl;
          // TODO do something?
        }
        w->perf_logger->tinc(l_msgr_running_total_time, dur);
      }
      w->reset();
      w->destroy();
  };
}

std::shared_ptr<NetworkStack> NetworkStack::create(CephContext *c,
						   const std::string &t)
{
  std::shared_ptr<NetworkStack> stack = nullptr;

  if (t == "posix")
    stack.reset(new PosixNetworkStack(c));
#ifdef HAVE_RDMA
  else if (t == "rdma")
    stack.reset(new RDMAStack(c));
#endif
#ifdef HAVE_DPDK
  else if (t == "dpdk")
    stack.reset(new DPDKStack(c));
#endif

  if (stack == nullptr) {
    lderr(c) << __func__ << " ms_async_transport_type " << t <<
    " is not supported! " << dendl;
    ceph_abort();
    return nullptr;
  }
  stack->balance_interval = clock_type::now();
  
  const int InitEventNumber = 5000;
  for (unsigned worker_id = 0; worker_id < stack->num_workers; ++worker_id) {
    Worker *w = stack->create_worker(c, worker_id);
    int ret = w->center.init(InitEventNumber, worker_id, t);
    if (ret)
      throw std::system_error(-ret, std::generic_category());
    w->balance_handler = new C_balance_msg(stack, w);
    stack->workers.push_back(w);
  }

  return stack;
}

NetworkStack::NetworkStack(CephContext *c)
  : cct(c)
{
  ceph_assert(cct->_conf->ms_async_op_threads > 0);

  num_workers = cct->_conf->ms_async_op_threads;
  if (num_workers >= EventCenter::MAX_EVENTCENTER) {
    ldout(cct, 0) << __func__ << " max thread limit is "
                  << EventCenter::MAX_EVENTCENTER << ", switching to this now. "
                  << "Higher thread values are unnecessary and currently unsupported."
                  << dendl;
    num_workers = EventCenter::MAX_EVENTCENTER;
  }
}

void NetworkStack::start()
{
  std::unique_lock<decltype(pool_spin)> lk(pool_spin);

  if (started) {
    return ;
  }

  for (unsigned i = 0; i < num_workers; ++i) {
    if (workers[i]->is_init())
      continue;
    std::function<void ()> thread = add_thread(i);
    spawn_worker(i, std::move(thread));
  }
  started = true;
  lk.unlock();

  for (unsigned i = 0; i < num_workers; ++i)
    workers[i]->wait_for_init();
}

Worker* NetworkStack::get_worker()
{
  ldout(cct, 30) << __func__ << dendl;

   // start with some reasonably large number
  unsigned min_load = std::numeric_limits<int>::max();
  Worker* current_best = nullptr;

  pool_spin.lock();
  // find worker with least references
  // tempting case is returning on references == 0, but in reality
  // this will happen so rarely that there's no need for special case.
  for (unsigned i = 0; i < num_workers; ++i) {
    unsigned worker_load = workers[i]->references.load();
    if (worker_load < min_load) {
      current_best = workers[i];
      min_load = worker_load;
    }
  }

  pool_spin.unlock();
  ceph_assert(current_best);
  ++current_best->references;
  return current_best;
}

void NetworkStack::stop()
{
  std::lock_guard lk(pool_spin);
  for (unsigned i = 0; i < num_workers; ++i) {
    workers[i]->done = true;
    workers[i]->center.wakeup();
    join_worker(i);
  }
  started = false;
}

class C_drain : public EventCallback {
  ceph::mutex drain_lock = ceph::make_mutex("C_drain::drain_lock");
  ceph::condition_variable drain_cond;
  unsigned drain_count;

 public:
  explicit C_drain(size_t c)
      : drain_count(c) {}
  void do_request(uint64_t id) override {
    std::lock_guard l{drain_lock};
    drain_count--;
    if (drain_count == 0) drain_cond.notify_all();
  }
  void wait() {
    std::unique_lock l{drain_lock};
    drain_cond.wait(l, [this] { return drain_count == 0; });
  }
};

void NetworkStack::drain()
{
  ldout(cct, 30) << __func__ << " started." << dendl;
  pthread_t cur = pthread_self();
  pool_spin.lock();
  C_drain drain(num_workers);
  for (unsigned i = 0; i < num_workers; ++i) {
    ceph_assert(cur != workers[i]->center.get_owner());
    workers[i]->center.dispatch_event_external(EventCallbackRef(&drain));
  }
  pool_spin.unlock();
  drain.wait();
  ldout(cct, 30) << __func__ << " end." << dendl;
}

uint64_t Worker::calculate_loads(std::map<AsyncConnection *, uint64_t> &loads) {
  std::unique_lock<std::mutex> locker(conns_lock);
  uint64_t sum = 0;
  for (auto conn: conns) {
    uint64_t load = conn->get_and_clear_load();
    if (load == 0)
      continue;
    loads[conn] = load;
    sum += load;
  }
  return sum;
}

void NetworkStack::balance(void)
{
  static pthread_t tid = 0;
  
  if(__sync_bool_compare_and_swap(&tid, 0, pthread_self()) == 0)
    return;
  if (migrating || clock_type::now() < balance_interval) {
    tid = 0;
    return;
  }
  
  std::map<AsyncConnection *, uint64_t> conn_loads_map;
  Worker * w_max = nullptr;
  Worker * w_min = nullptr;
  uint64_t load_max = 0;
  uint64_t load_min = std::numeric_limits<int>::max();
  uint64_t avg_load = 0;
  unsigned num_workers = get_num_worker();
  pool_spin.lock();
  for (unsigned i = 0; i < num_workers; i++) {
    uint64_t sum;
    std::map<AsyncConnection *, uint64_t> loads;
    sum = workers[i]->calculate_loads(loads);
    if (sum > load_max) {
      load_max = sum;
      w_max = workers[i];
      conn_loads_map.swap(loads);
    }
    loads.clear();
    if (sum < load_min) {
      load_min = sum;
      w_min = workers[i];
    }
    avg_load += sum / num_workers;
  }
  pool_spin.unlock();
  
  balance_interval = clock_type::now() + std::chrono::milliseconds(cct->_conf->ms_async_balance_ms);
  uint64_t delta = load_max - load_min;
  if (!load_max || (delta < cct->_conf->ms_async_balance_ratio * load_max)) {
    conn_loads_map.clear();
    tid = 0;
    return;
  }
  #define pow2(x) ((x) * (x))
  uint64_t org_stdval = pow2(load_max - avg_load) + pow2(load_min - avg_load);
  uint64_t min_stdval = org_stdval;
  AsyncConnection *conn_opt = nullptr;
  uint64_t conn_load = 0;
  for (auto it = conn_loads_map.begin(); it != conn_loads_map.end(); it++) {
    uint64_t load = it->second;
    if (load >= delta)
      continue;
    uint64_t stdval = pow2(load_max - avg_load - load) + pow2(load_min - avg_load + load);
    if (stdval < min_stdval) {
      conn_opt = it->first;
      min_stdval = stdval;
      conn_load = load;
    }
  }
  conn_loads_map.clear();
  if (org_stdval == min_stdval) {
    tid = 0;
    return;
  }
  if (w_max->conns.find(conn_opt) != w_max->conns.end()) {
    conn_opt->set_dest_worker(w_min);
    migrating = 1;
  } else {
    ldout(cct, 0) << __func__ << " conns " << conn_opt << " is deregisted, do nothing." << dendl;
  }
  ldout(cct, 1) << __func__ << " stdval " << org_stdval << " new stdval " << min_stdval << " conn_opt " << conn_opt << " conn_laod " << conn_load << " num conns " << w_max->conns.size() << dendl;
  tid = 0;
}
