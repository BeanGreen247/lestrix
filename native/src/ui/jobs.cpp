/*
 * Copyright (c) 2026 BeanGreen247
 * SPDX-License-Identifier: MIT
 */

#include "jobs.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int NWORKERS = 6;

struct Job {
    std::function<void()> work, done;
};

/* Leaked on purpose: detached workers may still be waking up while the process exits, so nothing here may be destroyed. */
std::mutex &mu = *new std::mutex;
std::condition_variable &cv = *new std::condition_variable;
std::deque<Job> &queued = *new std::deque<Job>;
std::deque<Job> &finished = *new std::deque<Job>;
std::vector<std::thread> &workers = *new std::vector<std::thread>;
bool quitting;
int busy;  /* workers running a job; guarded by mu */
std::atomic<int> pending;
void (*wake_fn)();

void worker() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(mu);
            cv.wait(lk, [] { return !queued.empty() || quitting; });
            if (queued.empty()) return;
            j = std::move(queued.front());
            queued.pop_front();
            busy++;
        }
        if (j.work) j.work();
        {
            std::lock_guard<std::mutex> lk(mu);
            busy--;
            finished.push_back(std::move(j));
        }
        if (wake_fn) wake_fn();
    }
}

}

/* Threads are started when work arrives (up to NWORKERS), so a window that never runs a job has none. */
void jobs_init(void (*wake)()) { wake_fn = wake; }

void jobs_shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu);
        quitting = true;
    }
    cv.notify_all();
    for (std::thread &t : workers) t.detach();
    workers.clear();
}

void jobs_run(std::function<void()> work, std::function<void()> done) {
    pending.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(mu);
        queued.push_back({std::move(work), std::move(done)});
        if (workers.size() < (size_t)NWORKERS && queued.size() > workers.size() - (size_t)busy) {
            try { workers.emplace_back(worker); } catch (...) {}
        }
    }
    cv.notify_one();
}

int jobs_pending() { return pending.load(); }

bool jobs_pump() {
    bool any = false;
    for (;;) {
        Job j;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (finished.empty()) break;
            j = std::move(finished.front());
            finished.pop_front();
        }
        if (j.done) j.done();
        pending.fetch_sub(1);
        any = true;
    }
    return any;
}
