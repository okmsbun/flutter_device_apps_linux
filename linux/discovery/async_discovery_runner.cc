#include "async_discovery_runner.h"

#include <exception>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace flutter_device_apps_linux {

namespace {

enum class DiscoveryRequestMode {
  kRaw,
  kResolved,
};

struct PendingDiscoveryRequest {
  DiscoveryRequestMode mode = DiscoveryRequestMode::kRaw;
  std::shared_ptr<std::atomic<bool>> cancelled;
  DiscoveryRequestOptions options;
  bool include_appstream_metadata = true;
  AsyncDiscoveryRunner::CompletionCallback on_raw_complete;
  AsyncDiscoveryRunner::ResolvedCompletionCallback on_resolved_complete;

  void notify_immediately(DiscoveryExecutionStatus status,
                          const std::string& error = "") {
    if (mode == DiscoveryRequestMode::kRaw) {
      if (!on_raw_complete) return;
      DiscoveryExecutionOutcome outcome;
      outcome.status = status;
      outcome.error_message = error;
      on_raw_complete(std::move(outcome));
    } else {
      if (!on_resolved_complete) return;
      ResolvedDiscoveryExecutionOutcome outcome;
      outcome.status = status;
      outcome.error_message = error;
      on_resolved_complete(std::move(outcome));
    }
  }
};

}  // namespace

DiscoveryRequestHandle::DiscoveryRequestHandle(
    std::shared_ptr<std::atomic<bool>> cancelled)
    : cancelled_(std::move(cancelled)) {}

void DiscoveryRequestHandle::cancel() {
  if (cancelled_ != nullptr) {
    cancelled_->store(true);
  }
}

bool DiscoveryRequestHandle::is_cancelled() const {
  return cancelled_ != nullptr && cancelled_->load();
}

struct AsyncDiscoveryRunner::State {
  std::shared_ptr<ApplicationDiscoveryService> service;
  WorkScheduler schedule_worker;
  CompletionDispatcher dispatch_completion;

  mutable std::mutex mutex;
  std::queue<PendingDiscoveryRequest> queue;
  bool worker_active = false;
  bool shutting_down = false;
};

AsyncDiscoveryRunner::AsyncDiscoveryRunner(
    std::shared_ptr<ApplicationDiscoveryService> service,
    WorkScheduler schedule_worker,
    CompletionDispatcher dispatch_completion)
    : state_(std::make_shared<State>()) {
  state_->service = std::move(service);
  state_->schedule_worker = std::move(schedule_worker);
  state_->dispatch_completion = std::move(dispatch_completion);
}

AsyncDiscoveryRunner::~AsyncDiscoveryRunner() {
  shutdown();
}

namespace {

template <typename StatePtr>
DiscoveryRequestHandle enqueue_request(
    const StatePtr& state, PendingDiscoveryRequest request,
    void (*worker_entry)(const StatePtr&)) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  request.cancelled = cancelled;
  DiscoveryRequestHandle handle(cancelled);

  bool rejected_shutdown = false;
  bool should_start_worker = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->shutting_down) {
      rejected_shutdown = true;
    } else {
      state->queue.push(std::move(request));
      if (!state->worker_active) {
        state->worker_active = true;
        should_start_worker = true;
      }
    }
  }

  if (rejected_shutdown) {
    cancelled->store(true);
    request.notify_immediately(DiscoveryExecutionStatus::kCancelled);
    return handle;
  }

  if (should_start_worker) {
    auto retained_state = state;
    try {
      if (!retained_state->schedule_worker) {
        throw std::runtime_error("Discovery worker scheduler is unavailable");
      }
      retained_state->schedule_worker(
          [retained_state, worker_entry]() { worker_entry(retained_state); });
    } catch (const std::exception& error) {
      std::vector<PendingDiscoveryRequest> failed;
      {
        std::lock_guard<std::mutex> lock(retained_state->mutex);
        retained_state->worker_active = false;
        while (!retained_state->queue.empty()) {
          failed.push_back(std::move(retained_state->queue.front()));
          retained_state->queue.pop();
        }
      }
      for (auto& pending : failed) {
        pending.notify_immediately(DiscoveryExecutionStatus::kExecutionError,
                                   error.what());
      }
    } catch (...) {
      std::vector<PendingDiscoveryRequest> failed;
      {
        std::lock_guard<std::mutex> lock(retained_state->mutex);
        retained_state->worker_active = false;
        while (!retained_state->queue.empty()) {
          failed.push_back(std::move(retained_state->queue.front()));
          retained_state->queue.pop();
        }
      }
      for (auto& pending : failed) {
        pending.notify_immediately(DiscoveryExecutionStatus::kExecutionError,
                                   "Unable to schedule discovery worker");
      }
    }
  }

  return handle;
}

}  // namespace

DiscoveryRequestHandle AsyncDiscoveryRunner::discover_async(
    CompletionCallback on_complete, DiscoveryRequestOptions options) {
  PendingDiscoveryRequest request;
  request.mode = DiscoveryRequestMode::kRaw;
  request.options = std::move(options);
  request.on_raw_complete = std::move(on_complete);
  return enqueue_request(state_, std::move(request), &run_worker_loop);
}

DiscoveryRequestHandle AsyncDiscoveryRunner::discover_resolved_async(
    ResolvedCompletionCallback on_complete, DiscoveryRequestOptions options,
    bool include_appstream_metadata) {
  PendingDiscoveryRequest request;
  request.mode = DiscoveryRequestMode::kResolved;
  request.options = std::move(options);
  request.include_appstream_metadata = include_appstream_metadata;
  request.on_resolved_complete = std::move(on_complete);
  return enqueue_request(state_, std::move(request), &run_worker_loop);
}

void AsyncDiscoveryRunner::shutdown() {
  std::vector<PendingDiscoveryRequest> drained;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->shutting_down) return;
    state_->shutting_down = true;
    while (!state_->queue.empty()) {
      auto request = std::move(state_->queue.front());
      state_->queue.pop();
      if (request.cancelled != nullptr) {
        request.cancelled->store(true);
      }
      drained.push_back(std::move(request));
    }
  }

  // Release queued requests immediately on the caller's thread without
  // blocking on any in-flight worker execution.
  for (auto& request : drained) {
    request.notify_immediately(DiscoveryExecutionStatus::kCancelled);
  }
}

bool AsyncDiscoveryRunner::is_shutting_down() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->shutting_down;
}

void AsyncDiscoveryRunner::run_worker_loop(
    const std::shared_ptr<State>& state) {
  while (true) {
    PendingDiscoveryRequest request;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (state->shutting_down || state->queue.empty()) {
        state->worker_active = false;
        return;
      }
      request = std::move(state->queue.front());
      state->queue.pop();
    }

    const bool cancelled_before_run =
        request.cancelled != nullptr && request.cancelled->load();

    if (request.mode == DiscoveryRequestMode::kRaw) {
      DiscoveryExecutionOutcome outcome;
      if (cancelled_before_run) {
        outcome.status = DiscoveryExecutionStatus::kCancelled;
      } else if (state->service == nullptr) {
        outcome.status = DiscoveryExecutionStatus::kCompleted;
      } else {
        try {
          DiscoveryPassResult pass =
              state->service->discover_with_report(request.options);
          outcome.status = DiscoveryExecutionStatus::kCompleted;
          outcome.applications = std::move(pass.applications);
          outcome.packages = std::move(pass.packages);
          outcome.provider_summaries = std::move(pass.provider_summaries);
        } catch (const std::exception& error) {
          outcome.status = DiscoveryExecutionStatus::kExecutionError;
          outcome.error_message = error.what();
        } catch (...) {
          outcome.status = DiscoveryExecutionStatus::kExecutionError;
          outcome.error_message = "Unexpected discovery execution failure";
        }
      }

      bool cancelled_after_run =
          request.cancelled != nullptr && request.cancelled->load();
      if (!cancelled_after_run) {
        std::lock_guard<std::mutex> lock(state->mutex);
        cancelled_after_run = state->shutting_down;
      }
      if (cancelled_after_run) {
        outcome.status = DiscoveryExecutionStatus::kCancelled;
        outcome.applications.clear();
        outcome.packages.clear();
        outcome.provider_summaries.clear();
        outcome.error_message.clear();
      }

      if (request.on_raw_complete && state->dispatch_completion) {
        auto callback = std::make_shared<CompletionCallback>(
            std::move(request.on_raw_complete));
        auto shared_outcome =
            std::make_shared<DiscoveryExecutionOutcome>(std::move(outcome));
        state->dispatch_completion([callback, shared_outcome]() {
          if (*callback) {
            (*callback)(std::move(*shared_outcome));
          }
        });
      }
    } else {
      ResolvedDiscoveryExecutionOutcome outcome;
      if (cancelled_before_run) {
        outcome.status = DiscoveryExecutionStatus::kCancelled;
      } else if (state->service == nullptr) {
        outcome.status = DiscoveryExecutionStatus::kCompleted;
      } else {
        try {
          outcome.inventory = state->service->discover_resolved(
              request.options, request.include_appstream_metadata);
          outcome.status = DiscoveryExecutionStatus::kCompleted;
        } catch (const std::exception& error) {
          outcome.status = DiscoveryExecutionStatus::kExecutionError;
          outcome.error_message = error.what();
        } catch (...) {
          outcome.status = DiscoveryExecutionStatus::kExecutionError;
          outcome.error_message = "Unexpected discovery execution failure";
        }
      }

      bool cancelled_after_run =
          request.cancelled != nullptr && request.cancelled->load();
      if (!cancelled_after_run) {
        std::lock_guard<std::mutex> lock(state->mutex);
        cancelled_after_run = state->shutting_down;
      }
      if (cancelled_after_run) {
        outcome.status = DiscoveryExecutionStatus::kCancelled;
        outcome.inventory = ResolvedDiscoveryInventory{};
        outcome.error_message.clear();
      }

      if (request.on_resolved_complete && state->dispatch_completion) {
        auto callback = std::make_shared<ResolvedCompletionCallback>(
            std::move(request.on_resolved_complete));
        auto shared_outcome =
            std::make_shared<ResolvedDiscoveryExecutionOutcome>(
                std::move(outcome));
        state->dispatch_completion([callback, shared_outcome]() {
          if (*callback) {
            (*callback)(std::move(*shared_outcome));
          }
        });
      }
    }
  }
}

}  // namespace flutter_device_apps_linux
