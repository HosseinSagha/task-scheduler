#pragma once

#include <stdint.h>

[[maybe_unused]] constexpr uint32_t Scheduler_stopPeriodicTask = UINT32_MAX; ///< Special value to stop a periodic task.

/// Scheduler task priority levels.
/// Tasks with Front priorities are added to the front of the queue of the priority level.
/// Do not use front priorities for the tasks that must be executed in the same order as they are added. The execution
/// order may not be preserved.
typedef enum
{
    Scheduler_TaskPriority_low,
    Scheduler_TaskPriority_medium,
    Scheduler_TaskPriority_high,
    Scheduler_TaskPriority_veryHigh,
    Scheduler_TaskPriority_levels, // regular priority level count

    Scheduler_TaskPriority_lowFront = Scheduler_TaskPriority_levels,
    Scheduler_TaskPriority_mediumFront,
    Scheduler_TaskPriority_highFront,
    Scheduler_TaskPriority_veryHighFront,

    Scheduler_TaskPriority_immediate, ///< Not queued and executed directly from main.
    Scheduler_TaskPriority_interrupt, ///< Not queued and executed directly from interrupt (for short interrupt
                                      ///< handlers).
    Scheduler_TaskPriority_none,      ///< For no task e.g. null dma interrupt callback in spi session.
} Scheduler_TaskPriority;

/// Task callback function type.
typedef void Scheduler_TaskNoCtxFn_t();
typedef void (*Scheduler_TaskNoCtxCb_t)();
typedef void Scheduler_TaskWithCtxFn_t(void *const ctx1, void *const ctx2);
typedef void (*Scheduler_TaskWithCtxCb_t)(void *const ctx1, void *const ctx2);

typedef void Scheduler_EnterSleepModeFn_t();
typedef void (*Scheduler_EnterSleepModeCb_t)();

/// Scheduler interface type.
typedef struct Scheduler *SchedulerIface;

struct Scheduler
{
    void *vars; ///< Pointer to scheduler variables.

    /// Timer interrupt handler to be called from the timer ISR.
    /// \param scheduler Scheduler interface.
    void (*timerIsrHandler)(const SchedulerIface scheduler);

    /// Add a periodic task to the scheduler (not interrupt-safe).
    /// \param scheduler Scheduler interface.
    /// \param taskNoCtxCb Callback function for the task.
    /// \param taskPriority Priority of the task.
    /// \param period_ms Period of the task in milliseconds. period_ms=0 rounds up to 1 tick, period_ms>0 rounds up to
    /// nearest tick to ensure full ticks, period_ms=Scheduler_stopPeriodicTask creates a one-shot task that fires once
    /// after initialDelay_ms and auto-stops (initialDelay_ms must be >0), or a fully stopped task (initialDelay_ms=0).
    /// \param initialDelay_ms Initial delay before the first execution in milliseconds. initialDelay_ms=0 fires on
    /// the next tick (no explicit delay). initialDelay_ms>0 rounds up to nearest tick to ensure at least that many
    /// full ticks pass before the first execution.
    /// \return Task ID on success (>=1, taskID=0 is reserved for uninitialised), -1 if the periodic task list is
    ///         full or if period_ms/initialDelay_ms tick calculation overflows int32_t.
    [[nodiscard]] int32_t (*addPeriodicTask)(const SchedulerIface scheduler,
                                             const Scheduler_TaskNoCtxCb_t taskNoCtxCb,
                                             const Scheduler_TaskPriority taskPriority,
                                             const uint32_t period_ms,
                                             const uint32_t initialDelay_ms);

    /// Add a periodic task to the scheduler with context pointers (not interrupt-safe).
    /// \param scheduler Scheduler interface.
    /// \param taskWithCtxCb Callback function for the task.
    /// \param taskPriority Priority of the task.
    /// \param period_ms Period of the task in milliseconds. period_ms=0 rounds up to 1 tick, period_ms>0 rounds up to
    /// nearest tick to ensure full ticks, period_ms=Scheduler_stopPeriodicTask creates a one-shot task that fires once
    /// after initialDelay_ms and auto-stops (initialDelay_ms must be >0), or a fully stopped task (initialDelay_ms=0).
    /// \param initialDelay_ms Initial delay before the first execution in milliseconds. initialDelay_ms=0 fires on
    /// the next tick (no explicit delay). initialDelay_ms>0 rounds up to nearest tick to ensure at least that many
    /// full ticks pass before the first execution.
    /// \param ctx1 optional context pointer 1 for the task.
    /// \param ctx2 optional context pointer 2 for the task.
    /// \return Task ID on success (>=1, taskID=0 is reserved for uninitialised), -1 if the periodic task list is
    ///         full or if period_ms/initialDelay_ms tick calculation overflows int32_t.
    [[nodiscard]] int32_t (*addPeriodicTaskWithCtx)(const SchedulerIface scheduler,
                                                    const Scheduler_TaskWithCtxCb_t taskWithCtxCb,
                                                    const Scheduler_TaskPriority taskPriority,
                                                    const uint32_t period_ms,
                                                    const uint32_t initialDelay_ms,
                                                    void *const ctx1,
                                                    void *const ctx2);

    /// Update the period of an existing periodic task. It can be called from the task itself.
    /// Task must be stopped first (period_ms=Scheduler_stopPeriodicTask) before changing to a new running period.
    /// \param scheduler Scheduler interface.
    /// \param taskId Task ID returned from addPeriodicTask or addPeriodicTaskWithCtx. taskId=0 is treated as
    /// uninitialised (no-op success), taskId<0 is invalid.
    /// \param newPeriod_ms New period in milliseconds. period_ms=0 rounds up to 1 tick, period_ms>0 rounds up to
    /// nearest tick to ensure full ticks, period_ms=Scheduler_stopPeriodicTask to stop the task.
    /// \param initialDelay_ms Initial delay before the first execution in milliseconds when starting a stopped task.
    /// \return true if the period was updated successfully or taskId=0 (uninitialised). false if taskId is out of
    ///         range, if the task is currently running and the new period is not Scheduler_stopPeriodicTask, or if
    ///         tick calculation overflows.
    [[nodiscard]] bool (*updatePeriodicTask)(const SchedulerIface scheduler,
                                             const int32_t taskId,
                                             const uint32_t newPeriod_ms,
                                             const uint32_t initialDelay_ms);

    /// Add a task to the scheduler queue from a timer interrupt.
    /// \param scheduler Scheduler interface.
    /// \param taskNoCtxCb Callback function for the task. User must ensure interrupt-safety of callback, if addTask is
    /// called from an ISR and taskPriority=Scheduler_TaskPriority_interrupt.
    /// \param taskPriority Priority of the task.
    /// \return true if the task was added to the queue, false if the queue was full or if the task was executed
    ///         directly (Scheduler_TaskPriority_immediate or Scheduler_TaskPriority_interrupt).
    [[nodiscard]] bool (*addTask)(const SchedulerIface scheduler,
                                  const Scheduler_TaskNoCtxCb_t taskNoCtxCb,
                                  const Scheduler_TaskPriority taskPriority);

    /// Add a task to the scheduler queue from a timer interrupt passing context pointers.
    /// \param scheduler Scheduler interface.
    /// \param taskWithCtxCb Callback function for the task. User must ensure interrupt-safety of callback, if
    /// addTaskWithCtx is called from an ISR and taskPriority=Scheduler_TaskPriority_interrupt.
    /// \param taskPriority Priority of the task.
    /// \param ctx1 optional context pointer 1 for the task. For Scheduler_TaskPriority_interrupt, any variable that may
    /// change by taskWithCtxCb and read in main thread, or vice versa, must be declared volatile.
    /// \param ctx2 optional context pointer 2 for the task. For Scheduler_TaskPriority_interrupt, any variable that may
    /// change by taskWithCtxCb and read in main thread, or vice versa, must be declared volatile.
    /// \return true if the task was added to the queue, false if the queue was full or if the task was executed
    ///         directly (Scheduler_TaskPriority_immediate or Scheduler_TaskPriority_interrupt).
    [[nodiscard]] bool (*addTaskWithCtx)(const SchedulerIface scheduler,
                                         const Scheduler_TaskWithCtxCb_t taskWithCtxCb,
                                         const Scheduler_TaskPriority taskPriority,
                                         void *const ctx1,
                                         void *const ctx2);

    /// Run the scheduler in the main loop (not for ISR).
    /// \param scheduler Scheduler interface.
    void (*run)(const SchedulerIface scheduler);
};

/// Creates a scheduler instance (not interrupt-safe).
/// \param tickPeriod_ms Scheduler tick period in milliseconds.
/// \param enterSleepModeCb Callback function to enter sleep mode.
/// \return Instance of the scheduler, or nullptr if maximum number of schedulers is reached or tickPeriod_ms=0.
SchedulerIface Scheduler_create(const uint32_t tickPeriod_ms, const Scheduler_EnterSleepModeCb_t enterSleepModeCb);
