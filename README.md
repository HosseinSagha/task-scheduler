# task-scheduler

Baremetal C task scheduler with priority queuing for repetitive and one-shot tasks, designed for ARM Cortex-M microcontrollers.

## Overview

The scheduler runs cooperatively in the main loop. A hardware timer ISR decrements countdown timers for periodic tasks and queues them when their deadline arrives. The main loop dequeues and executes one task per `run()` call, in priority order.

```
Hardware Timer ISR                    Main Loop
┌───────────────────┐                ┌───────────────────┐
│ timerIsrHandler() │                │ while (true)      │
│  for each task:   │                │   scheduler.run() │
│    --ticksUntilGo │   enqueue      │     dequeue       │
│    if ready ──────┼───────────────►│     execute task  │
│                   │                │     clear pending │
└───────────────────┘                └───────────────────┘
```

## Priority Levels

| Priority | Behaviour |
|---|---|
| `low`, `medium`, `high`, `veryHigh` | Queued. Dequeued highest-first by `run()`. |
| `lowFront` … `veryHighFront` | Same four levels, but inserted at the **front** of their queue (LIFO order within that level). |
| `immediate` | Executed inline by the caller of `addTask()` — not queued. |
| `interrupt` | Executed inline inside the ISR that calls `addTask()` — not queued. Use only for very short handlers. |

A starvation-prevention quota (default 10) limits consecutive high-priority executions, guaranteeing lower-priority tasks eventually run.

## Queuing and Execution Model

Each of the four base priority levels has its own circular buffer (power-of-2 size, default 32 slots with 31 usable).

### What happens on each timer tick

1. The ISR iterates over all registered periodic tasks.
2. For each task, `ticksUntilGo` is decremented (with saturation at `INT32_MIN+1` to avoid collision with the stop sentinel `INT32_MIN`).
3. When `ticksUntilGo` reaches 0, the task is enqueued — unless it is still pending from a previous fire (its callback has not finished yet).
4. After enqueuing, the countdown is reset using **absolute timing**: `ticksUntilGo += periodTicks`. This maintains a consistent schedule even if execution was delayed.

### What happens on each `run()` call

1. If no tasks are pending, the optional sleep callback is invoked and `run()` returns.
2. Otherwise, the highest-priority non-empty queue is selected (subject to the starvation quota).
3. One task is dequeued, the critical section exits, and the callback executes outside the lock.
4. After the callback returns, `isPending` is cleared so the ISR can re-enqueue the task.

### Sources of Delay

A queued task's actual execution time is affected by:

- **Queue depth**: all higher-priority tasks ahead of it must execute first.
- **Task execution time**: each `run()` call processes exactly one task. A long-running callback delays everything behind it.
- **Interrupt latency**: ISRs that fire during `run()` extend the wall-clock time of the current task.
- **Tick quantisation**: periods and delays are rounded up to whole ticks. A 1 ms period with a 10 ms tick becomes 10 ms.
- **Pending stall**: if a periodic task's callback is still running when its next deadline arrives, the ISR skips re-enqueuing and lets `ticksUntilGo` go negative. The task fires as soon as the previous execution clears `isPending`, and absolute timing recovers the schedule.

## One-Shot (Delayed Single-Fire) Tasks

To create a task that fires exactly once after a delay:

```c
scheduler->addPeriodicTask(scheduler, myCallback,
    Scheduler_TaskPriority_medium,
    Scheduler_stopPeriodicTask,  // period = stop → one-shot
    500);                        // fires once after 500 ms
```

When `period_ms = Scheduler_stopPeriodicTask` and `initialDelay_ms > 0`, the ISR counts down normally, enqueues the task once, then auto-stops it (sets `ticksUntilGo = INT32_MIN`). No self-stop logic is needed in the callback.

A stopped one-shot task can be re-armed later via `updatePeriodicTask()`.

## Watchdog Kicking Strategy

The scheduler's one-shot mechanism provides a clean pattern for kicking a watchdog: the kick callback re-arms itself as a delayed one-shot, ensuring the watchdog is only kicked if the scheduler is alive and running tasks on time.

### Setup

```c
static int32_t wdgTaskId;

void kickWatchdog(void) {
    Watchdog_kick();

    // Re-arm: kick again after delay
    scheduler->updatePeriodicTask(scheduler, wdgTaskId,
        Scheduler_stopPeriodicTask, WDG_KICK_DELAY_MS);
}

// At init — first kick after WDG_KICK_DELAY_MS
wdgTaskId = scheduler->addPeriodicTask(scheduler, kickWatchdog,
    Scheduler_TaskPriority_veryHighFront,
    Scheduler_stopPeriodicTask, WDG_KICK_DELAY_MS);
```

The watchdog task **must** use `veryHighFront` priority so it jumps ahead of all other queued tasks and executes as soon as `run()` is called.

### Windowed Watchdog Timing

A windowed watchdog has an **open window** `[T_open, T_close]` after the last kick. Kicking too early (before `T_open`) or too late (after `T_close`) triggers a reset.

```
        ◄── closed ──►◄──── open window ────►◄── timeout ──►
kick ─────────────────┬─────────────────────┬───────────────── reset
                   T_open               T_close
                      ▲
                  kick here
```

The kick delay must place the kick inside the open window. Because the scheduler is cooperative, the actual kick time is:

```
T_kick = WDG_KICK_DELAY_MS + T_queue + T_worst_task + T_worst_isr
```

Where:

- **`WDG_KICK_DELAY_MS`**: the configured one-shot delay (set this to at least `T_open`).
- **`T_queue`**: time for higher-priority or same-priority tasks already in the queue to execute. With `veryHighFront` and no other `veryHighFront` tasks, this is 0.
- **`T_worst_task`**: worst-case execution time of any single task callback in the system. Because `run()` processes one task atomically, a new `veryHighFront` task enqueued mid-execution must wait for the current task to finish.
- **`T_worst_isr`**: worst-case total nested interrupt time that can occur during one `run()` call.

The constraint for a windowed watchdog kicked at the **start** of the open window:

```
T_open  ≤  WDG_KICK_DELAY_MS
T_open  ≤  WDG_KICK_DELAY_MS + T_worst_task + T_worst_isr  ≤  T_close
```

Simplified:

```
WDG_KICK_DELAY_MS  ≥  T_open
T_worst_task + T_worst_isr  ≤  T_close − WDG_KICK_DELAY_MS
```

If you set `WDG_KICK_DELAY_MS = T_open`, then:

```
T_worst_task + T_worst_isr  ≤  T_close − T_open   (= open window width)
```

**The longest task execution time plus the longest nested interrupt time must not exceed the open window width.**

### Non-Windowed Watchdog Timing

A non-windowed (standard) watchdog simply resets if not kicked within `T_timeout` after the last kick. There is no "too early" constraint.

```
kick ────────────────────────────────────────── reset
                                             T_timeout
              ▲
          kick here (anywhere before T_timeout)
```

The constraint:

```
WDG_KICK_DELAY_MS + T_worst_task + T_worst_isr  ≤  T_timeout
```

Where `T_worst_task + T_worst_isr` is the worst-case latency between the one-shot delay expiring and the kick actually executing, same as the windowed case.

Choose `WDG_KICK_DELAY_MS` to leave enough margin:

```
WDG_KICK_DELAY_MS  ≤  T_timeout − T_worst_task − T_worst_isr
```

A typical choice is `WDG_KICK_DELAY_MS = T_timeout / 2`, which gives `T_timeout / 2` of margin for execution jitter.

### Why `veryHighFront`?

Using `veryHighFront` ensures:

1. The kick task is dequeued before all `veryHigh`, `high`, `medium`, and `low` tasks.
2. Front-insertion means it jumps ahead of other `veryHigh` tasks already in the queue.
3. `T_queue` is effectively 0 — only the currently executing task (if any) can delay it.

If a lower priority were used, `T_queue` would include the execution time of all higher-priority tasks in the queue, making the timing analysis much harder and the margins much tighter.

## API Reference

### `Scheduler_create(tickPeriod_ms, enterSleepModeCb)`

Creates a scheduler instance. `tickPeriod_ms` sets the timer tick granularity. `enterSleepModeCb` is called when no tasks are pending (pass `NULL` to busy-wait).

### `addPeriodicTask` / `addPeriodicTaskWithCtx`

Register a periodic task. Returns a task ID (≥1) or -1 on failure. The `WithCtx` variant passes two `void*` context pointers to the callback.

### `updatePeriodicTask(scheduler, taskId, newPeriod_ms, initialDelay_ms)`

Change the period of an existing task. The task must be stopped first (`Scheduler_stopPeriodicTask`) before setting a new running period. Can be called from the task's own callback.

### `addTask` / `addTaskWithCtx`

Enqueue a one-off task at a given priority. Returns `true` if queued, `false` if the queue was full or if the task was executed directly (`immediate`/`interrupt` priority).

### `timerIsrHandler`

Call this from your hardware timer ISR every `tickPeriod_ms`.

### `run`

Call this in your main loop. Executes one pending task per call, or invokes the sleep callback if idle.
