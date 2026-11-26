#ifndef FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_ASYNC_DISCOVERY_RUNNER_H_
#define FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_ASYNC_DISCOVERY_RUNNER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "application_discovery_service.h"

namespace flutter_device_apps_linux {

// High-level execution status for an asynchronous discovery request.
enum class DiscoveryExecutionStatus {
  kCompleted,
  kCancelled,
  kExecutionError,
};

// Owning outcome returned across the worker boundary to the completion context.
struct DiscoveryExecutionOutcome {
  DiscoveryExecutionStatus status = DiscoveryExecutionStatus::kCompleted;
  std::vector<DiscoveredApplication> applications;
  std::vector<InstalledPackageRecord> packages;
  std::vector<ProviderExecutionSummary> provider_summaries;
  std::string error_message;
};

// Owning outcome for unified resolved discovery across all selected providers.
struct ResolvedDiscoveryExecutionOutcome {
  DiscoveryExecutionStatus status = DiscoveryExecutionStatus::kCompleted;
  ResolvedDiscoveryInventory inventory;
  std::string error_message;
};

// Lightweight handle allowing best-effort cancellation of a submitted request.
// Cancellation never invalidates data currently accessed by an active worker.
class DiscoveryRequestHandle {
 public:
  DiscoveryRequestHandle() = default;
  explicit DiscoveryRequestHandle(std::shared_ptr<std::atomic<bool>> cancelled);

  void cancel();
  bool is_cancelled() const;

 private:
  std::shared_ptr<std::atomic<bool>> cancelled_;
};

// Serialized asynchronous execution boundary for ApplicationDiscoveryService.
//
// Concurrency and lifecycle guarantees:
// - At most one worker execution runs at a time, draining queued discovery
//   requests in FIFO order so providers are never invoked concurrently and
//   worker creation remains bounded.
// - Platform-thread calls (discover_async, shutdown) acquire only a short-lived
//   queue lock and never block waiting for an in-flight discovery pass.
// - The worker retains shared ownership of the runner state and
//   ApplicationDiscoveryService until any in-flight pass completes.
// - Completion callbacks are always dispatched through the caller-provided
//   completion dispatcher (the platform GMainContext in the Linux plugin).
class AsyncDiscoveryRunner {
 public:
  using CompletionCallback = std::function<void(DiscoveryExecutionOutcome)>;
  using ResolvedCompletionCallback =
      std::function<void(ResolvedDiscoveryExecutionOutcome)>;
  using WorkScheduler = std::function<void(std::function<void()>)>;
  using CompletionDispatcher = std::function<void(std::function<void()>)>;

  AsyncDiscoveryRunner(
      std::shared_ptr<ApplicationDiscoveryService> service,
      WorkScheduler schedule_worker,
      CompletionDispatcher dispatch_completion);
  ~AsyncDiscoveryRunner();

  AsyncDiscoveryRunner(const AsyncDiscoveryRunner&) = delete;
  AsyncDiscoveryRunner& operator=(const AsyncDiscoveryRunner&) = delete;
  AsyncDiscoveryRunner(AsyncDiscoveryRunner&&) = delete;
  AsyncDiscoveryRunner& operator=(AsyncDiscoveryRunner&&) = delete;

  // Enqueues an asynchronous discovery request. Every accepted request invokes
  // on_complete at most once on the completion context.
  DiscoveryRequestHandle discover_async(
      CompletionCallback on_complete,
      DiscoveryRequestOptions options =
          DiscoveryRequestOptions::all_providers());

  // Enqueues an asynchronous resolved discovery request backed by
  // ApplicationDiscoveryService::discover_resolved() on the same serialized
  // FIFO worker queue and safe teardown lifecycle as discover_async().
  DiscoveryRequestHandle discover_resolved_async(
      ResolvedCompletionCallback on_complete,
      DiscoveryRequestOptions options =
          DiscoveryRequestOptions::all_providers(),
      bool include_appstream_metadata = true);

  // Initiates non-blocking teardown. Queued requests that have not started are
  // immediately cancelled on the caller's thread; any currently running worker
  // finishes safely with its own shared ownership and delivers a kCancelled
  // outcome to the completion dispatcher without blocking teardown.
  void shutdown();

  bool is_shutting_down() const;

 private:
  struct State;
  static void run_worker_loop(const std::shared_ptr<State>& state);

  std::shared_ptr<State> state_;
};

}  // namespace flutter_device_apps_linux

#endif  // FLUTTER_DEVICE_APPS_LINUX_DISCOVERY_ASYNC_DISCOVERY_RUNNER_H_
