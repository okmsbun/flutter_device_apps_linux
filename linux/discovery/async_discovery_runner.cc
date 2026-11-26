#include "async_discovery_runner.h"

#include <exception>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

namespace flutter_device_apps_linux {

namespace {

struct PendingDiscoveryRequest {
  std::shared_ptr<std::atomic<bool>> cancelled;
  DiscoveryRequestOptions options;
  AsyncDiscoveryRunner::CompletionCallback on_complete;
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

DiscoveryRequestHandle AsyncDiscoveryRunner::discover_async(
    CompletionCallback on_complete, DiscoveryRequestOptions options) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  DiscoveryRequestHandle handle(cancelled);

  bool rejected_shutdown = false;
  bool should_start_worker = false;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->shutting_down) {
      rejected_shutdown = true;
    } else {
      PendingDiscoveryRequest request;
      request.cancelled = cancelled;
      request.options = std::move(options);
      request.on_complete = std::move(on_complete);
      state_->queue.push(std::move(request));
      if (!state_->worker_active) {
        state_->worker_active = true;
        should_start_worker = true;
      }
    }
  }

  if (rejected_shutdown) {
    cancelled->store(true);
    if (on_complete) {
      DiscoveryExecutionOutcome outcome;
      outcome.status = DiscoveryExecutionStatus::kCancelled;
      on_complete(std::move(outcome));
    }
    return handle;
  }

  if (should_start_worker) {
    auto state = state_;
    try {
      if (!state->schedule_worker) {
        throw std::runtime_error("Discovery worker scheduler is unavailable");
      }
      state->schedule_worker([state]() { run_worker_loop(state); });
    } catch (const std::exception& error) {
      std::vector<PendingDiscoveryRequest> failed;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->worker_active = false;
        while (!state->queue.empty()) {
          failed.push_back(std::move(state->queue.front()));
          state->queue.pop();
        }
      }
      for (auto& request : failed) {
        if (!request.on_complete) continue;
        DiscoveryExecutionOutcome outcome;
        outcome.status = DiscoveryExecutionStatus::kExecutionError;
        outcome.error_message = error.what();
        request.on_complete(std::move(outcome));
      }
    } catch (...) {
      std::vector<PendingDiscoveryRequest> failed;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->worker_active = false;
        while (!state->queue.empty()) {
          failed.push_back(std::move(state->queue.front()));
          state->queue.pop();
        }
      }
      for (auto& request : failed) {
        if (!request.on_complete) continue;
        DiscoveryExecutionOutcome outcome;
        outcome.status = DiscoveryExecutionStatus::kExecutionError;
        outcome.error_message = "Unable to schedule discovery worker";
        request.on_complete(std::move(outcome));
      }
    }
  }

  return handle;
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
    if (!request.on_complete) continue;
    DiscoveryExecutionOutcome outcome;
    outcome.status = DiscoveryExecutionStatus::kCancelled;
    request.on_complete(std::move(outcome));
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

    DiscoveryExecutionOutcome outcome;
    const bool cancelled_before_run =
        request.cancelled != nullptr && request.cancelled->load();
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
      outcome.provider_summaries.clear();
      outcome.error_message.clear();
    }

    if (request.on_complete && state->dispatch_completion) {
      auto callback =
          std::make_shared<CompletionCallback>(std::move(request.on_complete));
      auto shared_outcome =
          std::make_shared<DiscoveryExecutionOutcome>(std::move(outcome));
      state->dispatch_completion([callback, shared_outcome]() {
        if (*callback) {
          (*callback)(std::move(*shared_outcome));
        }
      });
    }
  }
}

}  // namespace flutter_device_apps_linux
