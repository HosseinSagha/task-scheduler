/// @file main.c
/// @brief Comprehensive example demonstrating all task-scheduler APIs.
///
/// This example showcases:
///   - Scheduler creation with a sleep-mode callback
///   - 2 periodic tasks (LED blink at 500 ms, sensor read at 1000 ms)
///   - 1 periodic task with context (data logger at 2000 ms)
///   - 2 single-shot tasks (addTask and addTaskWithCtx)
///   - 1 watchdog periodic task that is stopped/restarted via updatePeriodicTask
///   - 1 one-shot delayed task (using Scheduler_stopPeriodicTask + initialDelay)
///   - Timer ISR handler integration
///   - All priority levels demonstrated

#include "task-scheduler/scheduler.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

// ---------------------------------------------------------------------------
// Platform stubs (replace with real HW on a target)
// ---------------------------------------------------------------------------

static volatile bool running = true;
static volatile uint32_t tickCount;

// ---------------------------------------------------------------------------
// Sleep-mode callback passed to Scheduler_create
// ---------------------------------------------------------------------------

static void enterSleepMode(void)
{
    // On real hardware this would enter a low-power mode (e.g. WFI).
    // The MCU wakes on the next timer interrupt.
    printf("[sleep] entering low-power mode\n");
}

// ---------------------------------------------------------------------------
// Periodic task 1 – LED blink (500 ms, high priority, no context)
// ---------------------------------------------------------------------------

static void ledBlinkTask(void)
{
    static bool ledState;
    ledState = !ledState;
    printf("[periodic] LED %s  (tick %" PRIu32 ")\n", ledState ? "ON " : "OFF", tickCount);
}

// ---------------------------------------------------------------------------
// Periodic task 2 – Sensor read (1000 ms, medium priority, no context)
// ---------------------------------------------------------------------------

static void sensorReadTask(void)
{
    const int32_t fakeTemp = 22 + (int32_t)(tickCount % 5);
    printf("[periodic] sensor temperature = %" PRId32 " °C  (tick %" PRIu32 ")\n", fakeTemp, tickCount);
}

// ---------------------------------------------------------------------------
// Periodic task 3 – Data logger with context (2000 ms, low priority)
// ---------------------------------------------------------------------------

typedef struct
{
    const char *name;
    uint32_t sampleCount;
} LoggerCtx;

static LoggerCtx loggerCtx = {.name = "DataLogger", .sampleCount = 0};

static void dataLoggerTask(void *const ctx1, void *const ctx2)
{
    LoggerCtx *const logger = (LoggerCtx *)ctx1;
    const uint32_t *const maxSamples = (const uint32_t *)ctx2;

    ++logger->sampleCount;
    printf("[periodic] %s: sample %" PRIu32 " / %" PRIu32 "  (tick %" PRIu32 ")\n",
           logger->name,
           logger->sampleCount,
           *maxSamples,
           tickCount);

    if (logger->sampleCount >= *maxSamples)
    {
        printf("[periodic] %s: max samples reached – task will keep running\n", logger->name);
    }
}

// ---------------------------------------------------------------------------
// Watchdog task – periodic, stopped at init, started/stopped via updatePeriodicTask
// ---------------------------------------------------------------------------

typedef struct
{
    SchedulerIface scheduler;
    int32_t taskId;
    uint32_t kickCount;
} WatchdogCtx;

static WatchdogCtx wdgCtx = {.scheduler = nullptr, .taskId = 0, .kickCount = 0};

static void watchdogKickTask(void *const ctx1, void *const ctx2)
{
    (void)ctx2;
    WatchdogCtx *const wdg = (WatchdogCtx *)ctx1;
    ++wdg->kickCount;
    printf("[watchdog] kick %" PRIu32 "!  (tick %" PRIu32 ")\n", wdg->kickCount, tickCount);
    // On real hardware: reload the hardware watchdog timer here

    // Self-stop after 7 kicks (~1400 ms at 200 ms period)
    if (wdg->kickCount >= 7)
    {
        if (wdg->scheduler->updatePeriodicTask(wdg->scheduler, wdg->taskId, Scheduler_stopPeriodicTask, 0))
        {
            printf("[watchdog] self-stopped after %" PRIu32 " kicks  (tick %" PRIu32 ")\n", wdg->kickCount, tickCount);
        }
    }
}

// ---------------------------------------------------------------------------
// One-shot delayed task – fires once 1500 ms after registration, then auto-stops
// ---------------------------------------------------------------------------

static void delayedInitTask()
{
    printf("[one-shot] delayed initialisation complete  (tick %" PRIu32 ")\n", tickCount);
}

// ---------------------------------------------------------------------------
// Single-shot queued tasks (addTask / addTaskWithCtx)
// ---------------------------------------------------------------------------

static void buttonPressHandler()
{
    printf("[single-shot] button press handled  (tick %" PRIu32 ")\n", tickCount);
}

static void dmaCompleteHandler(void *const ctx1, void *const ctx2)
{
    const uint32_t *const channel = (const uint32_t *)ctx1;
    const uint32_t *const bytesTransferred = (const uint32_t *)ctx2;
    printf("[single-shot] DMA ch%" PRIu32 " done – %" PRIu32 " bytes  (tick %" PRIu32 ")\n",
           *channel,
           *bytesTransferred,
           tickCount);
}

// ---------------------------------------------------------------------------
// Simulated timer ISR – call this at the tick rate (e.g. every 10 ms)
// ---------------------------------------------------------------------------

static void simulateTimerIsr(const SchedulerIface scheduler)
{
    ++tickCount;
    scheduler->timerIsrHandler(scheduler);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    // ---- 1. Create scheduler (10 ms tick) --------------------------------
    const SchedulerIface scheduler = Scheduler_create(10, enterSleepMode);
    if (scheduler == nullptr)
    {
        printf("ERROR: failed to create scheduler\n");
        return 1;
    }
    printf("scheduler created (tick = 10 ms)\n");

    // ---- 2. Periodic task: LED blink – 500 ms, high priority -------------
    const int32_t ledTaskId = scheduler->addPeriodicTask(scheduler, ledBlinkTask, Scheduler_TaskPriority_high, 500, 0);
    if (ledTaskId < 0)
    {
        printf("ERROR: failed to add LED task\n");
        return 1;
    }
    printf("added periodic LED task (id=%" PRId32 ", 500 ms)\n", ledTaskId);

    // ---- 3. Periodic task: sensor read – 1000 ms, medium priority --------
    const int32_t sensorTaskId =
        scheduler->addPeriodicTask(scheduler, sensorReadTask, Scheduler_TaskPriority_medium, 1000, 0);
    if (sensorTaskId < 0)
    {
        printf("ERROR: failed to add sensor task\n");
        return 1;
    }
    printf("added periodic sensor task (id=%" PRId32 ", 1000 ms)\n", sensorTaskId);

    // ---- 4. Periodic task with context: data logger – 2000 ms, low -------
    static uint32_t maxSamples = 5;
    const int32_t loggerTaskId = scheduler->addPeriodicTaskWithCtx(
        scheduler, dataLoggerTask, Scheduler_TaskPriority_low, 2000, 0, &loggerCtx, &maxSamples);
    if (loggerTaskId < 0)
    {
        printf("ERROR: failed to add logger task\n");
        return 1;
    }
    printf("added periodic logger task with context (id=%" PRId32 ", 2000 ms)\n", loggerTaskId);

    // ---- 5. Watchdog task – initially stopped, 200 ms when running -------
    wdgCtx.scheduler = scheduler;
    const int32_t wdgTaskId = scheduler->addPeriodicTaskWithCtx(
        scheduler, watchdogKickTask, Scheduler_TaskPriority_veryHigh, Scheduler_stopPeriodicTask, 0, &wdgCtx, nullptr);
    if (wdgTaskId < 0)
    {
        printf("ERROR: failed to add watchdog task\n");
        return 1;
    }
    wdgCtx.taskId = wdgTaskId;
    printf("added watchdog task (id=%" PRId32 ", initially stopped)\n", wdgTaskId);

    // ---- 6. One-shot delayed task – fires once after 1500 ms -------------
    const int32_t delayedTaskId = scheduler->addPeriodicTask(
        scheduler, delayedInitTask, Scheduler_TaskPriority_medium, Scheduler_stopPeriodicTask, 1500);
    if (delayedTaskId < 0)
    {
        printf("ERROR: failed to add delayed init task\n");
        return 1;
    }
    printf("added one-shot delayed task (id=%" PRId32 ", fires after 1500 ms)\n", delayedTaskId);

    // ---- 7. Start the watchdog after setup is done -----------------------
    if (!scheduler->updatePeriodicTask(scheduler, wdgTaskId, 200, 0))
    {
        printf("ERROR: failed to start watchdog\n");
        return 1;
    }
    printf("watchdog started (200 ms period)\n");

    // ---- 8. Main loop – simulate 300 ticks (3 seconds) -------------------
    printf("\n--- scheduler running ---\n\n");

    constexpr uint32_t totalTicks = 300;

    for (uint32_t i = 0; i < totalTicks; ++i)
    {
        // Simulate the timer ISR firing every tick
        simulateTimerIsr(scheduler);

        // -- Inject single-shot tasks at specific ticks --

        // At tick 50 (500 ms): simulate a button press (addTask, veryHigh front priority)
        if (tickCount == 50)
        {
            scheduler->addTask(scheduler, buttonPressHandler, Scheduler_TaskPriority_veryHighFront);
            printf("  >> queued button-press single-shot task at tick %" PRIu32 "\n", tickCount);
        }

        // At tick 80 (800 ms): simulate DMA complete (addTaskWithCtx, high priority)
        if (tickCount == 80)
        {
            static uint32_t dmaChannel = 2;
            static uint32_t dmaBytes = 256;
            scheduler->addTaskWithCtx(
                scheduler, dmaCompleteHandler, Scheduler_TaskPriority_high, &dmaChannel, &dmaBytes);
            printf("  >> queued DMA-complete single-shot task at tick %" PRIu32 "\n", tickCount);
        }

        // At tick 200 (2 s): restart the watchdog with a longer period (500 ms)
        if (tickCount == 200)
        {
            if (scheduler->updatePeriodicTask(scheduler, wdgTaskId, 500, 0))
            {
                printf("  >> watchdog restarted (500 ms) at tick %" PRIu32 "\n", tickCount);
            }
        }

        // Run the scheduler – dispatches one ready task per call
        scheduler->run(scheduler);
    }

    printf("\n--- example finished (%" PRIu32 " ticks) ---\n", totalTicks);
    return 0;
}
