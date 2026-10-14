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
        }
        if (j.work) j.work();
        {
            std::lock_guard<std::mutex> lk(mu);
            finished.push_back(std::move(j));
        }
        if (wake_fn) wake_fn();
    }
}

}

void jobs_init(void (*wake)()) {
    wake_fn = wake;
    for (int i = 0; i < NWORKERS; i++) {
        try { workers.emplace_back(worker); } catch (...) { break; }
    }
}

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
