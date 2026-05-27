#include "scheduler.h"
// #include "systemView.h"
#include "core.h"
#include "utility.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

constexpr size_t periodicTaskListSize = 20;

// Power of 2 for fast modulo operations (& instead of %)
constexpr size_t queueSize = 32;
constexpr size_t queueSizeMask = (queueSize - 1);
static_assert((queueSize & queueSizeMask) == 0, "QUEUE_SIZE must be a power of 2");

constexpr int32_t stopPeriodicTaskTicks = INT32_MIN;
constexpr int32_t minPendingTicks =
    (stopPeriodicTaskTicks + 1); // Saturation floor to prevent collision with stop sentinel

typedef union
{
    Scheduler_TaskNoCtxCb_t taskNoCtxCb;
    Scheduler_TaskWithCtxCb_t taskWithCtxCb;
} TaskCb;

typedef struct
{
    TaskCb taskCb;
    void *ctx1;     // Context pointer 1 for the task
    void *ctx2;     // Context pointer 2 for the task
    int32_t taskID; // ID of the periodic task, or 0 for non-periodic tasks
    bool hasCtx;
} TaskData;

typedef struct
{
    size_t head[Scheduler_TaskPriority_levels];
    size_t tail[Scheduler_TaskPriority_levels];
    TaskData tasksData[Scheduler_TaskPriority_levels][queueSize]; // Data for each task in the taskQueue
} TaskQueue;

typedef struct
{
    TaskCb taskCb;                   // Union of task callback function pointers
    void *ctx1;                      // Context pointer 1 for the task
    void *ctx2;                      // Context pointer 2 for the task
    volatile int32_t periodTicks;    // How often to run, in timer ticks
    volatile int32_t ticksUntilGo;   // Countdown timer (can go negative for absolute timing, saturated at INT32_MIN+1)
    Scheduler_TaskPriority priority; // Priority to run the task with
    volatile bool isPending;         // True if task is already queued but not executed yet
    bool hasCtx;                     // True if task has context pointers
} PeriodicTask;

typedef struct
{
    PeriodicTask periodicTasks[periodicTaskListSize]; // Large array first
    volatile TaskQueue taskQueue;                     // Large struct second
    Scheduler_EnterSleepModeCb_t enterSleepModeCb;    // function pointer
    volatile uint32_t pendingPriorities;              // Bitmask: bit N set = priority N has pending tasks
    volatile size_t periodicTaskCount;                // Read by ISR, written by main thread
    uint32_t tickPeriod_ms;                           // Tick period in milliseconds
    size_t highPriorityExecutionCount; // Consecutive high-priority task executions (protected by critical section)
} Vars, *const VarsPtr;

typedef struct
{
    struct Scheduler scheduler;
    Vars vars;
} Scheduler;

constexpr size_t maxSchedulers = 1;

Scheduler schedulers[maxSchedulers];

static int32_t addPeriodicTask(const SchedulerIface scheduler,
                               const Scheduler_TaskNoCtxCb_t taskNoCtxCb,
                               const Scheduler_TaskPriority taskPriority,
                               const uint32_t period_ms,
                               const uint32_t initialDelay_ms);
static int32_t addPeriodicTaskWithCtx(const SchedulerIface scheduler,
                                      const Scheduler_TaskWithCtxCb_t taskWithCtxCb,
                                      const Scheduler_TaskPriority taskPriority,
                                      const uint32_t period_ms,
                                      const uint32_t initialDelay_ms,
                                      void *const ctx1,
                                      void *const ctx2);

/// Convert period in milliseconds to ticks, rounding up to ensure at least the specified number of full ticks.
/// \param period_ms Period in milliseconds. period_ms=0 rounds up to 1 tick.
/// \param tickPeriod_ms Tick period in milliseconds.
/// \return Tick count (>=1) on success, 0 if the result would overflow int32_t.
static int32_t convertPeriodMsToTicks(uint32_t period_ms, uint32_t tickPeriod_ms) [[unsequenced]];

/// Convert initial delay in milliseconds to ticks until go, rounding up to ensure at least the specified number of
/// full ticks. The returned value includes one extra tick for the first ISR decrement so the task fires no earlier
/// than the requested delay.
/// \param initialDelay_ms Initial delay in milliseconds. 0 means fire on the next tick.
/// \param tickPeriod_ms Tick period in milliseconds.
/// \return Ticks until go (>=1) on success, 0 if the result would overflow int32_t.
static int32_t convertInitialDelayMsToTicksUntilGo(uint32_t initialDelay_ms,
                                                   uint32_t tickPeriod_ms) [[unsequenced]];

/// Add a periodic task to the scheduler's periodic task list.
/// This is an internal helper function called by both addPeriodicTask and addPeriodicTaskWithCtx.
/// \param scheduler Scheduler interface.
/// \param taskCb Callback function for the task.
/// \param taskPriority Priority of the task.
/// \param period_ms Period of the task in milliseconds. period_ms=Scheduler_stopPeriodicTask with initialDelay_ms>0
///        creates a one-shot task that fires once after the delay and auto-stops; with initialDelay_ms=0 creates a
///        fully stopped task.
/// \param initialDelay_ms Initial delay before the first execution in milliseconds.
/// \param hasCtx True if the task has context pointers.
/// \param ctx1 optional context pointer 1 for the task.
/// \param ctx2 optional context pointer 2 for the task.
/// \return Task ID on success (>=1, taskID=0 is reserved), -1 if the periodic task list is full or if
///         period_ms/initialDelay_ms tick calculation overflows int32_t.
static int32_t addToPeriodicList(const SchedulerIface scheduler,
                                 const TaskCb taskCb,
                                 const Scheduler_TaskPriority taskPriority,
                                 const uint32_t period_ms,
                                 const uint32_t initialDelay_ms,
                                 const bool hasCtx,
                                 void *const ctx1,
                                 void *const ctx2);
static void timerIsrHandler(const SchedulerIface scheduler);
static bool addTask(const SchedulerIface scheduler,
                    const Scheduler_TaskNoCtxCb_t taskNoCtxCb,
                    const Scheduler_TaskPriority taskPriority);
static bool addTaskWithCtx(const SchedulerIface scheduler,
                           const Scheduler_TaskWithCtxCb_t taskWithCtxCb,
                           const Scheduler_TaskPriority taskPriority,
                           void *const ctx1,
                           void *const ctx2);

/// Add a task to the scheduler queue.
/// \param scheduler Scheduler interface.
/// \param taskCb Callback function for the task.
/// \param taskPriority Priority of the task.
/// \param hasCtx True if the task has context pointers.
/// \param ctx1 optional context pointer 1 for the task.
/// \param ctx2 optional context pointer 2 for the task.
/// \return true if the task was added to the queue, false if the queue was full or if the task was executed
///         directly (interrupt/immediate priority).
static bool addToQueue(const SchedulerIface scheduler,
                       const TaskCb taskCb,
                       Scheduler_TaskPriority taskPriority,
                       const int32_t taskID,
                       const bool hasCtx,
                       void *const ctx1,
                       void *const ctx2);
static bool updatePeriodicTask(const SchedulerIface scheduler,
                               const int32_t taskID,
                               const uint32_t newPeriod_ms,
                               const uint32_t initialDelay_ms);
static void run(const SchedulerIface scheduler);

SchedulerIface Scheduler_create(const uint32_t tickPeriod_ms, const Scheduler_EnterSleepModeCb_t enterSleepModeCb)
{
    static size_t schedulerIndex;

    if (UNLIKELY(schedulerIndex >= maxSchedulers || tickPeriod_ms == 0))
    {
        assert(schedulerIndex < maxSchedulers);
        assert(tickPeriod_ms != 0);
        return nullptr;
    }

    auto scheduler = &schedulers[schedulerIndex].scheduler;
    auto varsPtr = &schedulers[schedulerIndex].vars;
    ++schedulerIndex;

    scheduler->vars = varsPtr;
    varsPtr->tickPeriod_ms = tickPeriod_ms;
    varsPtr->enterSleepModeCb = enterSleepModeCb;

    scheduler->addPeriodicTask = addPeriodicTask;
    scheduler->addPeriodicTaskWithCtx = addPeriodicTaskWithCtx;
    scheduler->updatePeriodicTask = updatePeriodicTask;
    scheduler->timerIsrHandler = timerIsrHandler;
    scheduler->addTask = addTask;
    scheduler->addTaskWithCtx = addTaskWithCtx;
    scheduler->run = run;

    return scheduler;
}

static int32_t addPeriodicTask(const SchedulerIface scheduler,
                               const Scheduler_TaskNoCtxCb_t taskNoCtxCb,
                               const Scheduler_TaskPriority taskPriority,
                               const uint32_t period_ms,
                               const uint32_t initialDelay_ms)
{
    assert(taskNoCtxCb != nullptr);

    return addToPeriodicList(scheduler,
                             (TaskCb){.taskNoCtxCb = taskNoCtxCb},
                             taskPriority,
                             period_ms,
                             initialDelay_ms,
                             false,
                             nullptr,
                             nullptr);
}

static int32_t addPeriodicTaskWithCtx(const SchedulerIface scheduler,
                                      const Scheduler_TaskWithCtxCb_t taskWithCtxCb,
                                      const Scheduler_TaskPriority taskPriority,
                                      const uint32_t period_ms,
                                      const uint32_t initialDelay_ms,
                                      void *const ctx1,
                                      void *const ctx2)
{
    assert(taskWithCtxCb != nullptr);

    return addToPeriodicList(scheduler,
                             (TaskCb){.taskWithCtxCb = taskWithCtxCb},
                             taskPriority,
                             period_ms,
                             initialDelay_ms,
                             true,
                             ctx1,
                             ctx2);
}

static int32_t addToPeriodicList(const SchedulerIface scheduler,
                                 const TaskCb taskCb,
                                 const Scheduler_TaskPriority taskPriority,
                                 const uint32_t period_ms,
                                 const uint32_t initialDelay_ms,
                                 const bool hasCtx,
                                 void *const ctx1,
                                 void *const ctx2)
{
    assert(taskPriority < Scheduler_TaskPriority_none);

    const VarsPtr restrict varsPtr = (Vars *)scheduler->vars;

    // Precalculate period ticks outside critical section to minimize lock time
    int32_t periodTicks;
    int32_t ticksUntilGo;
    const uint32_t tickPeriod = varsPtr->tickPeriod_ms;

    if (UNLIKELY(period_ms == Scheduler_stopPeriodicTask))
    {
        periodTicks = stopPeriodicTaskTicks;

        // One-shot task: compute delay from initialDelay_ms, auto-stops after firing.
        // If initialDelay_ms is also 0, task starts fully stopped (existing behaviour).
        if (initialDelay_ms == 0U)
        {
            ticksUntilGo = stopPeriodicTaskTicks;
        }
        else
        {
            ticksUntilGo = convertInitialDelayMsToTicksUntilGo(initialDelay_ms, tickPeriod);
            if (UNLIKELY(ticksUntilGo == 0))
            {
                return -1;
            }
        }
    }
    else
    {
        // Calculate ticks: ensure at least the specified number of full ticks pass.
        periodTicks = convertPeriodMsToTicks(period_ms, tickPeriod);
        ticksUntilGo = convertInitialDelayMsToTicksUntilGo(initialDelay_ms, tickPeriod);
        if (UNLIKELY(periodTicks == 0 || ticksUntilGo == 0))
        {
            return -1;
        }
    }

    // Enter critical section to atomically check and update task count and initialize task
    Core_criticalSectionEnter();

    const size_t taskIndex = varsPtr->periodicTaskCount;
    if (UNLIKELY(taskIndex >= periodicTaskListSize))
    {
        Core_criticalSectionExit();
        return -1;
    }

    // Initialize task while protected by critical section
    PeriodicTask *restrict taskPtr = &varsPtr->periodicTasks[taskIndex];
    taskPtr->taskCb = taskCb;
    taskPtr->priority = taskPriority;
    taskPtr->periodTicks = periodTicks;
    taskPtr->ticksUntilGo = ticksUntilGo;
    taskPtr->hasCtx = hasCtx;
    taskPtr->ctx1 = ctx1;
    taskPtr->ctx2 = ctx2;
    taskPtr->isPending = false;

    // Atomically increment counter with all fields initialized
    ++varsPtr->periodicTaskCount;

    Core_criticalSectionExit();
    // Return 1-based taskID (taskID=0 is reserved as sentinel value)
    return (int32_t)(taskIndex + 1);
}

static bool updatePeriodicTask(const SchedulerIface scheduler,
                               const int32_t taskID,
                               const uint32_t newPeriod_ms,
                               const uint32_t initialDelay_ms)
{
    // Validate taskID
    if (UNLIKELY(taskID == 0))
    {
        return true; // Uninitialized task, treat as success
    }
    else if (UNLIKELY(taskID < 0))
    {
        return false;
    }

    // Convert 1-based taskID to 0-based array index
    const size_t taskIndex = (size_t)(taskID - 1);
    const VarsPtr restrict varsPtr = scheduler->vars;

    if (UNLIKELY(taskIndex >= varsPtr->periodicTaskCount))
    {
        return false;
    }

    int32_t newTicks;
    int32_t ticksUntilGo = 0;
    if (UNLIKELY(newPeriod_ms == Scheduler_stopPeriodicTask))
    {
        newTicks = stopPeriodicTaskTicks;
    }
    else
    {
        // Calculate new period
        const uint32_t tickPeriod = varsPtr->tickPeriod_ms;
        newTicks = convertPeriodMsToTicks(newPeriod_ms, tickPeriod);
        ticksUntilGo = convertInitialDelayMsToTicksUntilGo(initialDelay_ms, tickPeriod);
        if (UNLIKELY(newTicks == 0 || ticksUntilGo == 0))
        {
            return false;
        }
    }

    PeriodicTask *restrict const taskPtr = &varsPtr->periodicTasks[taskIndex];
    const int32_t oldPeriodTicks = taskPtr->periodTicks;

    // Early exit if no change
    if (UNLIKELY(newTicks == oldPeriodTicks))
    {
        return true;
    }

    // Task must be stopped before changing period (unless we're stopping it now)
    if (oldPeriodTicks != stopPeriodicTaskTicks && newTicks != stopPeriodicTaskTicks)
    {
        return false;
    }

    Core_criticalSectionEnter();

    // Update period
    taskPtr->periodTicks = newTicks;

    // Update ticksUntilGo based on operation
    if (newTicks == stopPeriodicTaskTicks)
    {
        // Stopping task
        taskPtr->ticksUntilGo = stopPeriodicTaskTicks;
    }
    else
    {
        // Starting task from stopped state
        taskPtr->ticksUntilGo = ticksUntilGo;
    }

    Core_criticalSectionExit();

    return true;
}

static int32_t convertPeriodMsToTicks(const uint32_t period_ms, const uint32_t tickPeriod_ms) [[unsequenced]] 
{
    const uint32_t periodTicks_u32 = (period_ms == 0U) ? 1U : ((period_ms - 1U) / tickPeriod_ms + 1U);
    if (UNLIKELY(periodTicks_u32 > (uint32_t)INT32_MAX))
    {
        return 0; // Overflow sentinel
    }

    return (int32_t)periodTicks_u32;
}

static int32_t convertInitialDelayMsToTicksUntilGo(const uint32_t initialDelay_ms,
                                                                    const uint32_t tickPeriod_ms) [[unsequenced]] 
{
    const uint32_t initialDelayTicks_u32 =
        (initialDelay_ms == 0U) ? 0U : ((initialDelay_ms - 1U) / tickPeriod_ms + 1U);

    // Add one extra count for the first decrement tick.
    if (UNLIKELY(initialDelayTicks_u32 >= (uint32_t)INT32_MAX))
    {
        return 0; // Overflow sentinel
    }

    return (int32_t)(initialDelayTicks_u32 + 1U);
}

static void timerIsrHandler(const SchedulerIface scheduler)
{
    const VarsPtr restrict varsPtr = scheduler->vars;

    // Loop only through active periodic tasks (not the entire array)
    const size_t taskCount = varsPtr->periodicTaskCount;
    PeriodicTask *restrict periodicTaskPtr = varsPtr->periodicTasks;

    for (size_t taskIndex = 0; taskIndex < taskCount; ++taskIndex, ++periodicTaskPtr)
    {
        // Cache volatile field into local to avoid repeated volatile reads
        int32_t ticks = periodicTaskPtr->ticksUntilGo;

        // Early exit for stopped tasks (using signed comparison)
        if (UNLIKELY(ticks == stopPeriodicTaskTicks))
        {
            continue;
        }

        // Decrement with saturation to avoid signed overflow and collision with stop sentinel.
        if (LIKELY(ticks > minPendingTicks))
        {
            --ticks;
        }
        else if (UNLIKELY(ticks < minPendingTicks))
        {
            ticks = minPendingTicks;
        }

        // Check if task should run (deadline reached or passed)
        if (UNLIKELY(ticks <= 0))
        {
            // Pre-load period ticks to avoid cache miss during reset
            const int32_t periodTicks = periodicTaskPtr->periodTicks;

            // Only queue if not already pending (prevents overflow during blocking operations)
            if (LIKELY(!periodicTaskPtr->isPending))
            {
                const int32_t taskID = (int32_t)(taskIndex + 1); // 1-based taskID

                // Queue task; set isPending afterwards to prevent re-queuing before execution completes
                const bool queued = addToQueue(scheduler,
                                               periodicTaskPtr->taskCb,
                                               periodicTaskPtr->priority,
                                               taskID,
                                               periodicTaskPtr->hasCtx,
                                               periodicTaskPtr->ctx1,
                                               periodicTaskPtr->ctx2);
                periodicTaskPtr->isPending = queued;

                // One-shot task: auto-stop after firing
                if (UNLIKELY(periodTicks == stopPeriodicTaskTicks))
                {
                    ticks = stopPeriodicTaskTicks;
                }
                else
                {
                    // Absolute timing: add period to current ticks (which may be negative)
                    // This maintains absolute schedule even when delayed
                    ticks += periodTicks;

                    // Safety: if severely delayed (>1 period), limit catch-up to prevent avalanche
                    if (UNLIKELY(ticks <= 0))
                    {
                        ticks = periodTicks;
                    }
                }
            }
            // else: task still pending, keep ticking down (will accumulate negative delay)
        }

        // Write back tick counter
        periodicTaskPtr->ticksUntilGo = ticks;
    }
}

static bool addTask(const SchedulerIface scheduler,
                    const Scheduler_TaskNoCtxCb_t taskNoCtxCb,
                    const Scheduler_TaskPriority taskPriority)
{
    assert(taskNoCtxCb != nullptr);

    return addToQueue(scheduler, (TaskCb){.taskNoCtxCb = taskNoCtxCb}, taskPriority, 0, false, nullptr, nullptr);
}

static bool addTaskWithCtx(const SchedulerIface scheduler,
                           const Scheduler_TaskWithCtxCb_t taskWithCtxCb,
                           const Scheduler_TaskPriority taskPriority,
                           void *const ctx1,
                           void *const ctx2)
{
    assert(taskWithCtxCb != nullptr);

    return addToQueue(scheduler, (TaskCb){.taskWithCtxCb = taskWithCtxCb}, taskPriority, 0, true, ctx1, ctx2);
}

static bool addToQueue(const SchedulerIface scheduler,
                       const TaskCb taskCb,
                       Scheduler_TaskPriority taskPriority,
                       const int32_t taskID,
                       const bool hasCtx,
                       void *const ctx1,
                       void *const ctx2)
{
    assert(taskPriority < Scheduler_TaskPriority_none);

    // Handle immediate priority tasks without queueing
    if (UNLIKELY(taskPriority == Scheduler_TaskPriority_interrupt || taskPriority == Scheduler_TaskPriority_immediate))
    {
        if (LIKELY(hasCtx))
        {
            taskCb.taskWithCtxCb(ctx1, ctx2);
        }
        else
        {
            taskCb.taskNoCtxCb();
        }

        return false; // Not queued, but executed directly
    }

    // Handle front-queue priorities (convert front priorities to base priorities)
    const bool addFront = UNLIKELY(taskPriority >= Scheduler_TaskPriority_levels);
    if (addFront)
    {
        taskPriority = (Scheduler_TaskPriority)(taskPriority - Scheduler_TaskPriority_levels);
    }

    const VarsPtr restrict varsPtr = scheduler->vars;
    volatile TaskQueue *const restrict queuePtr = &varsPtr->taskQueue;

    Core_criticalSectionEnter();

    // Direct pointer access for better performance
    volatile size_t *const restrict headPtr = &queuePtr->head[taskPriority];
    volatile size_t *const restrict tailPtr = &queuePtr->tail[taskPriority];
    const size_t head = *headPtr;
    const size_t tail = *tailPtr;

    // Check if queue is full
    const size_t nextHead = (head + 1) & queueSizeMask;
    if (UNLIKELY(nextHead == tail))
    {
        Core_criticalSectionExit();
        assert(false); // Queue overflow, should be sized appropriately for the application
        return false;
    }

    // Compute index and update pointer atomically
    size_t writeIndex;
    if (UNLIKELY(addFront))
    {
        writeIndex = (tail + queueSize - 1) & queueSizeMask;
        *tailPtr = writeIndex;
    }
    else
    {
        writeIndex = head;
        *headPtr = nextHead;
    }

    // Write task data - compiler will optimize struct copy
    volatile TaskData *const restrict taskDataPtr = &queuePtr->tasksData[taskPriority][writeIndex];
    taskDataPtr->taskCb = taskCb;
    taskDataPtr->taskID = taskID;
    taskDataPtr->ctx1 = ctx1;
    taskDataPtr->ctx2 = ctx2;
    taskDataPtr->hasCtx = hasCtx;

    // Set priority bit
    varsPtr->pendingPriorities |= (1U << taskPriority);

    Core_criticalSectionExit();
    return true;
}

static void run(const SchedulerIface scheduler)
{
    const VarsPtr restrict varsPtr = scheduler->vars;
    volatile TaskQueue *const restrict queuePtr = &varsPtr->taskQueue;
    volatile size_t *const restrict heads = queuePtr->head;
    volatile size_t *const restrict tails = queuePtr->tail;

    // Check if any tasks are pending using bitmask
    Core_criticalSectionEnter();
    const uint32_t priorityBits = varsPtr->pendingPriorities;
    if (UNLIKELY(priorityBits == 0))
    {
        Core_criticalSectionExit();

        // No tasks pending, enter low power mode
        const Scheduler_EnterSleepModeCb_t enterSleepModeCb = varsPtr->enterSleepModeCb;
        if (LIKELY(enterSleepModeCb != nullptr))
        {
            enterSleepModeCb();
        }

        return;
    }

    //    SYSVIEW_EXIT_ISR_TO_SCHEDULER(); // Mark resuming from interrupt

    // Find highest priority task using CLZ (O(1) on ARM Cortex-M)
    const Scheduler_TaskPriority highestPriority = (Scheduler_TaskPriority)(31U - (uint32_t)COUNTL_ZERO(priorityBits));

    // Maximum consecutive high-priority task executions before forcing a lower-priority check
    // Prevents high-priority tasks from starving lower priorities indefinitely
    constexpr size_t highPriorityExecutionQuota = 10;

    // Compute final task priority with quota enforcement in one expression
    Scheduler_TaskPriority taskPriority = highestPriority;
    if (UNLIKELY(varsPtr->highPriorityExecutionCount >= highPriorityExecutionQuota &&
                 highestPriority >= Scheduler_TaskPriority_high))
    {
        // Check if lower priorities have pending tasks and select highest among them
        const uint32_t lowerPriorityBits = priorityBits & ((1U << Scheduler_TaskPriority_high) - 1);
        if (LIKELY(lowerPriorityBits != 0))
        {
            taskPriority = (Scheduler_TaskPriority)(31U - (uint32_t)COUNTL_ZERO(lowerPriorityBits));
        }
        // else: no lower priority tasks, keep highestPriority
    }

    // Get current head and tail for this priority (cache volatile reads)
    const size_t head = heads[taskPriority];
    const size_t tail = tails[taskPriority];
    const size_t newTail = (tail + 1) & queueSizeMask;

    // Copy task data - single cache line read
    volatile const TaskData *const restrict taskDataPtr = &queuePtr->tasksData[taskPriority][tail];
    const TaskCb taskCb = taskDataPtr->taskCb;
    const int32_t taskID = taskDataPtr->taskID;
    void *const ctx1 = taskDataPtr->ctx1;
    void *const ctx2 = taskDataPtr->ctx2;
    const bool hasCtx = taskDataPtr->hasCtx;

    // Update tail pointer
    tails[taskPriority] = newTail;

    // Clear priority bit if queue empty (using cached head value)
    if (UNLIKELY(head == newTail))
    {
        varsPtr->pendingPriorities &= ~(1U << taskPriority);
    }

    // Update quota counter inside critical section for proper synchronization
    if (LIKELY(taskPriority >= Scheduler_TaskPriority_high))
    {
        ++varsPtr->highPriorityExecutionCount;
    }
    else
    {
        varsPtr->highPriorityExecutionCount = 0;
    }

    Core_criticalSectionExit();

    // Execute task outside critical section
    if (LIKELY(hasCtx))
    {
        taskCb.taskWithCtxCb(ctx1, ctx2);
    }
    else
    {
        taskCb.taskNoCtxCb();
    }

    // Clear isPending flag for periodic tasks (atomic write, no critical section needed)
    if (LIKELY(taskID > 0))
    {
        const size_t taskIndex = (size_t)(taskID - 1);
        varsPtr->periodicTasks[taskIndex].isPending = false;
    }

    //    SYSVIEW_IDLE(); // Mark idle state after task completion
}
