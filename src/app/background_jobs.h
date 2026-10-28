#pragma once

#include "common/hr.h"
#include "common/log.h"

#include <unknwn.h>
#include <winrt/base.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <list>
#include <memory>
#include <thread>

// Долгие операции (сохранение повтора, PNG) вне главного потока, чтобы не ломать темп кадров.
// Деструктор дожидается всех задач.
class BackgroundJobs {
public:
    void Run(std::function<void()> fn)
    {
        Reap();
        auto done = std::make_shared<std::atomic<bool>>(false);
        jobs_.push_back({ std::thread([fn = std::move(fn), done] {
                              winrt::init_apartment(winrt::apartment_type::multi_threaded);
                              try {
                                  fn();
                              } catch (const winrt::hresult_error& e) {
                                  LogError("Фоновая задача: 0x%08X", static_cast<unsigned>(e.code()));
                              } catch (const std::exception& e) {
                                  LogError("Фоновая задача: %s", e.what());
                              }
                              winrt::uninit_apartment();
                              *done = true;
                          }),
                          done });
    }

    ~BackgroundJobs()
    {
        for (auto& j : jobs_)
            j.thread.join();
    }

private:
    struct Job {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void Reap()
    {
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (*it->done) {
                it->thread.join();
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::list<Job> jobs_;
};
