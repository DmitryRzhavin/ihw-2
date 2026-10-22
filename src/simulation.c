#define _POSIX_C_SOURCE 200809L

#include "simulation.h"

#include "config.h"
#include "logger.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_OPERATIONS \
    (MAX_VISITORS * MAX_DISHES_PER_ORDER * MAX_STEPS_PER_DISH)
#define MAX_READY_ITEMS (MAX_VISITORS * MAX_DISHES_PER_ORDER)
#define WAIT_POLL_MS 100U
#define SLEEP_CHUNK_MS 50U

typedef enum {
    ORDER_NEW = 0,
    ORDER_WAITING_WAITER,
    ORDER_SUBMITTED,
    ORDER_COOKING,
    ORDER_ASSEMBLED,
    ORDER_DELIVERED,
    ORDER_CANCELED
} OrderState;

typedef enum {
    OP_NOT_READY = 0,
    OP_READY,
    OP_QUEUED,
    OP_CLAIMED,
    OP_RUNNING,
    OP_DONE
} OperationState;

typedef struct {
    unsigned menu_index;
    unsigned operation_count;
    unsigned operation_ids[MAX_STEPS_PER_DISH];
    unsigned completed_operations;
} RuntimeDish;

typedef struct {
    unsigned visitor_id;
    unsigned waiter_id;
    unsigned dish_count;
    RuntimeDish dishes[MAX_DISHES_PER_ORDER];
    unsigned ready_dishes;
    uint64_t arrival_sequence;
    OrderState state;
    int runtime_created;
} RuntimeOrder;

typedef struct {
    unsigned id;
    unsigned order_index;
    unsigned dish_index;
    unsigned step_index;
    unsigned cook_id;
    EquipmentType equipment;
    unsigned duration;
    uint64_t ready_sequence;
    uint64_t queued_ms;
    OperationState state;
    int queue_full_reported;
} RuntimeOperation;

typedef struct {
    unsigned order_index;
    unsigned dish_index;
} ReadyDish;

typedef struct {
    unsigned visitors_arrived;
    unsigned orders_accepted;
    unsigned orders_delivered;
    unsigned orders_canceled;
    unsigned dishes_created;
    unsigned dishes_ready;
    unsigned operations_created;
    unsigned operations_started;
    unsigned operations_completed;
    uint64_t total_queue_wait_ms;
    uint64_t cook_busy_units;
    uint64_t equipment_busy_units[EQUIPMENT_TYPES];
} Statistics;

typedef struct Restaurant Restaurant;

typedef struct {
    Restaurant *restaurant;
    unsigned index;
} ThreadArg;

struct Restaurant {
    const Config *config;
    const SimulationOptions *options;
    Logger logger;

    pthread_mutex_t state_mutex;
    sem_t visitor_waiting;
    sem_t coordinator_events;
    sem_t kitchen_items;
    sem_t kitchen_slots;
    sem_t ready_slots;
    sem_t visitor_finished;
    sem_t equipment[EQUIPMENT_TYPES];
    sem_t *order_ready;
    sem_t *visitor_done;

    RuntimeOrder *orders;
    RuntimeOperation *operations;
    unsigned operation_count;
    unsigned *kitchen_queue;
    unsigned kitchen_count;
    ReadyDish *ready_queue;
    unsigned ready_count;

    pthread_t coordinator_thread;
    pthread_t *visitor_threads;
    pthread_t *waiter_threads;
    pthread_t *cook_threads;
    ThreadArg *visitor_args;
    ThreadArg *waiter_args;
    ThreadArg *cook_args;
    unsigned visitors_created;
    unsigned waiters_created;
    unsigned cooks_created;
    int coordinator_created;

    uint64_t next_ready_sequence;
    uint64_t next_arrival_sequence;
    uint64_t start_ms;
    Statistics stats;

    atomic_int shutdown;
    atomic_int failed;

    int logger_initialized;
    int mutex_initialized;
    unsigned base_semaphores_initialized;
    unsigned equipment_semaphores_initialized;
    unsigned order_semaphores_initialized;
};

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static uint64_t model_time(const Restaurant *restaurant)
{
    uint64_t elapsed = monotonic_ms() - restaurant->start_ms;
    return elapsed / restaurant->options->time_unit_ms;
}

static int stop_requested(const Restaurant *restaurant)
{
    if (atomic_load(&restaurant->shutdown) != 0)
        return 1;
    if (restaurant->options->stop_flag != NULL &&
        *restaurant->options->stop_flag != 0)
        return 1;
    return 0;
}

static void add_ms_to_timespec(struct timespec *time, unsigned ms)
{
    time->tv_sec += (time_t)(ms / 1000U);
    time->tv_nsec += (long)(ms % 1000U) * 1000000L;
    if (time->tv_nsec >= 1000000000L) {
        ++time->tv_sec;
        time->tv_nsec -= 1000000000L;
    }
}

/* Ожидание семафора с периодической проверкой завершения программы. */
static int wait_sem_interruptible(Restaurant *restaurant, sem_t *semaphore)
{
    for (;;) {
        struct timespec deadline;

        if (stop_requested(restaurant))
            return -1;
        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
            return -1;
        add_ms_to_timespec(&deadline, WAIT_POLL_MS);

        if (sem_timedwait(semaphore, &deadline) == 0)
            return 0;
        if (errno == EINTR || errno == ETIMEDOUT)
            continue;
        return -1;
    }
}

static int sleep_ms_interruptible(Restaurant *restaurant, uint64_t total_ms)
{
    uint64_t slept = 0;

    while (slept < total_ms) {
        uint64_t left = total_ms - slept;
        unsigned chunk = left > SLEEP_CHUNK_MS ? SLEEP_CHUNK_MS : (unsigned)left;
        struct timespec request;

        if (stop_requested(restaurant))
            return -1;

        request.tv_sec = (time_t)(chunk / 1000U);
        request.tv_nsec = (long)(chunk % 1000U) * 1000000L;
        while (nanosleep(&request, &request) != 0) {
            if (errno != EINTR)
                return -1;
            if (stop_requested(restaurant))
                return -1;
        }
        slept += chunk;
    }
    return 0;
}

static int sleep_units(Restaurant *restaurant, uint64_t units)
{
    uint64_t milliseconds = units * (uint64_t)restaurant->options->time_unit_ms;
    return sleep_ms_interruptible(restaurant, milliseconds);
}

static uint32_t next_random(uint32_t *state)
{
    uint32_t x = *state;

    if (x == 0)
        x = 0x9e3779b9U;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint64_t arrival_time_for(const Config *config, unsigned visitor_index)
{
    uint64_t result = 0;
    unsigned i;

    if (config->arrival_gap_count == 0)
        return (uint64_t)visitor_index * config->arrival_interval;

    for (i = 0; i <= visitor_index; ++i)
        result += config->arrival_gaps[i];
    return result;
}

static void prepare_orders(Restaurant *restaurant)
{
    const Config *config = restaurant->config;
    uint32_t rng = config->seed;
    unsigned visitor;

    for (visitor = 0; visitor < config->visitor_count; ++visitor) {
        RuntimeOrder *order = &restaurant->orders[visitor];
        unsigned dish;

        order->visitor_id = visitor + 1;
        order->state = ORDER_NEW;

        if (config->order_count != 0) {
            order->dish_count = config->order_dish_count[visitor];
            for (dish = 0; dish < order->dish_count; ++dish)
                order->dishes[dish].menu_index = config->order_dishes[visitor][dish];
        } else {
            unsigned used[MAX_DISHES] = {0};
            order->dish_count = config->dishes_per_order;
            for (dish = 0; dish < order->dish_count; ++dish) {
                unsigned menu_index;
                do {
                    menu_index = next_random(&rng) % config->menu_size;
                } while (used[menu_index] != 0);
                used[menu_index] = 1;
                order->dishes[dish].menu_index = menu_index;
            }
        }
    }
}

static void format_order(const Restaurant *restaurant,
                         const RuntimeOrder *order,
                         char *buffer,
                         size_t buffer_size)
{
    size_t used = 0;
    unsigned i;

    if (buffer_size == 0)
        return;
    buffer[0] = '\0';

    for (i = 0; i < order->dish_count; ++i) {
        const char *name = restaurant->config->menu[order->dishes[i].menu_index].name;
        int written = snprintf(buffer + used, buffer_size - used,
                               "%s%s", i == 0 ? "" : ", ", name);
        if (written < 0)
            return;
        if ((size_t)written >= buffer_size - used) {
            buffer[buffer_size - 1] = '\0';
            return;
        }
        used += (size_t)written;
    }
}

static int order_is_possible(const Restaurant *restaurant,
                             const RuntimeOrder *order,
                             EquipmentType *missing_equipment)
{
    unsigned dish;

    for (dish = 0; dish < order->dish_count; ++dish) {
        const DishSpec *spec = &restaurant->config->menu[order->dishes[dish].menu_index];
        unsigned step;

        for (step = 0; step < spec->operation_count; ++step) {
            EquipmentType type = spec->operations[step].equipment;
            if (restaurant->config->equipment_count[type] == 0) {
                if (missing_equipment != NULL)
                    *missing_equipment = type;
                return 0;
            }
        }
    }
    return 1;
}

static int choose_ready_operation_locked(const Restaurant *restaurant)
{
    int chosen = -1;
    unsigned i;

    for (i = 0; i < restaurant->operation_count; ++i) {
        const RuntimeOperation *operation = &restaurant->operations[i];

        if (operation->state != OP_READY)
            continue;
        if (chosen < 0) {
            chosen = (int)i;
            continue;
        }

        if (restaurant->config->priority == PRIORITY_FIFO) {
            if (operation->ready_sequence <
                restaurant->operations[(unsigned)chosen].ready_sequence)
                chosen = (int)i;
        } else if (operation->ready_sequence >
                   restaurant->operations[(unsigned)chosen].ready_sequence) {
            chosen = (int)i;
        }
    }
    return chosen;
}

static int find_unreported_ready_locked(Restaurant *restaurant)
{
    unsigned i;

    for (i = 0; i < restaurant->operation_count; ++i) {
        RuntimeOperation *operation = &restaurant->operations[i];
        if (operation->state == OP_READY && !operation->queue_full_reported) {
            operation->queue_full_reported = 1;
            return (int)i;
        }
    }
    return -1;
}

static void coordinator_enqueue_pending(Restaurant *restaurant)
{
    for (;;) {
        int operation_index;
        RuntimeOperation snapshot;
        const char *dish_name;

        if (sem_trywait(&restaurant->kitchen_slots) != 0) {
            if (errno == EAGAIN) {
                pthread_mutex_lock(&restaurant->state_mutex);
                operation_index = find_unreported_ready_locked(restaurant);
                if (operation_index >= 0)
                    snapshot = restaurant->operations[(unsigned)operation_index];
                pthread_mutex_unlock(&restaurant->state_mutex);

                if (operation_index >= 0) {
                    dish_name = restaurant->config->menu[
                        restaurant->orders[snapshot.order_index]
                            .dishes[snapshot.dish_index].menu_index].name;
                    logger_event(&restaurant->logger, model_time(restaurant),
                                 "Kitchen queue is full; operation %u of dish '%s' "
                                 "for order %u waits for a free slot",
                                 snapshot.step_index + 1, dish_name,
                                 snapshot.order_index + 1);
                }
            }
            return;
        }

        pthread_mutex_lock(&restaurant->state_mutex);
        operation_index = choose_ready_operation_locked(restaurant);
        if (operation_index < 0) {
            pthread_mutex_unlock(&restaurant->state_mutex);
            sem_post(&restaurant->kitchen_slots);
            return;
        }

        restaurant->operations[(unsigned)operation_index].state = OP_QUEUED;
        restaurant->operations[(unsigned)operation_index].queued_ms = monotonic_ms();
        restaurant->operations[(unsigned)operation_index].queue_full_reported = 0;
        restaurant->kitchen_queue[restaurant->kitchen_count++] =
            (unsigned)operation_index;
        snapshot = restaurant->operations[(unsigned)operation_index];
        pthread_mutex_unlock(&restaurant->state_mutex);

        dish_name = restaurant->config->menu[
            restaurant->orders[snapshot.order_index]
                .dishes[snapshot.dish_index].menu_index].name;
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Coordinator queued operation %u of dish '%s' for order %u",
                     snapshot.step_index + 1, dish_name,
                     snapshot.order_index + 1);
        sem_post(&restaurant->kitchen_items);
    }
}

static int create_submitted_order(Restaurant *restaurant)
{
    unsigned order_index;
    RuntimeOrder *order = NULL;
    EquipmentType missing = EQUIPMENT_STOVE;
    int possible = 1;
    unsigned first_operation_count = 0;
    unsigned first_operations[MAX_DISHES_PER_ORDER];

    pthread_mutex_lock(&restaurant->state_mutex);
    for (order_index = 0; order_index < restaurant->config->visitor_count;
         ++order_index) {
        if (restaurant->orders[order_index].state == ORDER_SUBMITTED &&
            !restaurant->orders[order_index].runtime_created) {
            order = &restaurant->orders[order_index];
            break;
        }
    }

    if (order == NULL) {
        pthread_mutex_unlock(&restaurant->state_mutex);
        return 0;
    }

    possible = order_is_possible(restaurant, order, &missing);
    order->runtime_created = 1;

    if (!possible) {
        order->state = ORDER_CANCELED;
        ++restaurant->stats.orders_canceled;
        pthread_mutex_unlock(&restaurant->state_mutex);

        logger_event(&restaurant->logger, model_time(restaurant),
                     "Coordinator canceled order %u: required equipment '%s' "
                     "is unavailable",
                     order_index + 1, config_equipment_name(missing));
        sem_post(&restaurant->order_ready[order_index]);
        return 1;
    }

    order->state = ORDER_COOKING;
    for (unsigned dish_index = 0; dish_index < order->dish_count; ++dish_index) {
        RuntimeDish *dish = &order->dishes[dish_index];
        const DishSpec *spec = &restaurant->config->menu[dish->menu_index];

        dish->operation_count = spec->operation_count;
        ++restaurant->stats.dishes_created;

        for (unsigned step = 0; step < spec->operation_count; ++step) {
            RuntimeOperation *operation;
            unsigned id = restaurant->operation_count++;

            operation = &restaurant->operations[id];
            memset(operation, 0, sizeof(*operation));
            operation->id = id;
            operation->order_index = order_index;
            operation->dish_index = dish_index;
            operation->step_index = step;
            operation->equipment = spec->operations[step].equipment;
            operation->duration = spec->operations[step].duration;
            operation->state = OP_NOT_READY;
            dish->operation_ids[step] = id;
            ++restaurant->stats.operations_created;
        }

        if (dish->operation_count > 0) {
            RuntimeOperation *first =
                &restaurant->operations[dish->operation_ids[0]];
            first->state = OP_READY;
            first->ready_sequence = ++restaurant->next_ready_sequence;
            first_operations[first_operation_count++] = first->id;
        }
    }
    pthread_mutex_unlock(&restaurant->state_mutex);

    logger_event(&restaurant->logger, model_time(restaurant),
                 "Coordinator created kitchen tasks for order %u",
                 order_index + 1);

    for (unsigned dish_index = 0; dish_index < order->dish_count; ++dish_index) {
        RuntimeDish *dish = &order->dishes[dish_index];
        const DishSpec *spec = &restaurant->config->menu[dish->menu_index];

        logger_event(&restaurant->logger, model_time(restaurant),
                     "Coordinator created dish '%s' for order %u with %u operation%s",
                     spec->name, order_index + 1, spec->operation_count,
                     spec->operation_count == 1 ? "" : "s");

        for (unsigned step = 0; step < spec->operation_count; ++step) {
            logger_event(&restaurant->logger, model_time(restaurant),
                         "Created operation %u for dish '%s': %s, duration=%u",
                         step + 1, spec->name,
                         config_equipment_name(spec->operations[step].equipment),
                         spec->operations[step].duration);
        }
    }

    (void)first_operations;
    (void)first_operation_count;
    return 1;
}

static int process_one_ready_dish(Restaurant *restaurant)
{
    ReadyDish item;
    RuntimeOrder *order;
    RuntimeDish *dish;
    const char *dish_name;
    int assembled = 0;

    pthread_mutex_lock(&restaurant->state_mutex);
    if (restaurant->ready_count == 0) {
        pthread_mutex_unlock(&restaurant->state_mutex);
        return 0;
    }

    item = restaurant->ready_queue[0];
    if (restaurant->ready_count > 1) {
        memmove(&restaurant->ready_queue[0], &restaurant->ready_queue[1],
                (restaurant->ready_count - 1) * sizeof(restaurant->ready_queue[0]));
    }
    --restaurant->ready_count;

    order = &restaurant->orders[item.order_index];
    dish = &order->dishes[item.dish_index];
    dish_name = restaurant->config->menu[dish->menu_index].name;
    ++order->ready_dishes;

    if (order->ready_dishes == order->dish_count &&
        order->state == ORDER_COOKING) {
        order->state = ORDER_ASSEMBLED;
        assembled = 1;
    }
    pthread_mutex_unlock(&restaurant->state_mutex);

    sem_post(&restaurant->ready_slots);
    logger_event(&restaurant->logger, model_time(restaurant),
                 "Coordinator took ready dish '%s' for order %u from ready area",
                 dish_name, item.order_index + 1);

    if (assembled) {
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Coordinator assembled order %u: all %u dishes are ready",
                     item.order_index + 1, order->dish_count);
        sem_post(&restaurant->order_ready[item.order_index]);
    }
    return 1;
}

/* Координатор создает операции, заполняет кухонную очередь и собирает заказы. */
static void *coordinator_thread_main(void *argument)
{
    Restaurant *restaurant = (Restaurant *)argument;

    logger_event(&restaurant->logger, model_time(restaurant),
                 "Kitchen coordinator started");

    while (!stop_requested(restaurant)) {
        if (wait_sem_interruptible(restaurant, &restaurant->coordinator_events) != 0)
            break;

        while (!stop_requested(restaurant) && create_submitted_order(restaurant)) {
        }
        while (!stop_requested(restaurant) && process_one_ready_dish(restaurant)) {
        }
        if (!stop_requested(restaurant))
            coordinator_enqueue_pending(restaurant);
    }

    logger_event(&restaurant->logger, model_time(restaurant),
                 "Kitchen coordinator stopped");
    return NULL;
}

static int pop_kitchen_operation(Restaurant *restaurant, unsigned *operation_index)
{
    unsigned index;

    pthread_mutex_lock(&restaurant->state_mutex);
    if (restaurant->kitchen_count == 0) {
        pthread_mutex_unlock(&restaurant->state_mutex);
        return -1;
    }

    if (restaurant->config->priority == PRIORITY_FIFO) {
        index = restaurant->kitchen_queue[0];
        if (restaurant->kitchen_count > 1) {
            memmove(&restaurant->kitchen_queue[0], &restaurant->kitchen_queue[1],
                    (restaurant->kitchen_count - 1) *
                        sizeof(restaurant->kitchen_queue[0]));
        }
    } else {
        index = restaurant->kitchen_queue[restaurant->kitchen_count - 1];
    }
    --restaurant->kitchen_count;
    restaurant->operations[index].state = OP_CLAIMED;
    *operation_index = index;
    pthread_mutex_unlock(&restaurant->state_mutex);

    sem_post(&restaurant->kitchen_slots);
    sem_post(&restaurant->coordinator_events);
    return 0;
}

/* Повар берет операцию из общей очереди и захватывает нужное оборудование. */
static void *cook_thread_main(void *argument)
{
    ThreadArg *arg = (ThreadArg *)argument;
    Restaurant *restaurant = arg->restaurant;
    unsigned cook_id = arg->index + 1;

    logger_event(&restaurant->logger, model_time(restaurant),
                 "Cook %u started shift", cook_id);

    while (!stop_requested(restaurant)) {
        unsigned operation_index;
        RuntimeOperation operation;
        const char *dish_name;
        int final_operation;

        if (wait_sem_interruptible(restaurant, &restaurant->kitchen_items) != 0)
            break;
        if (stop_requested(restaurant))
            break;
        if (pop_kitchen_operation(restaurant, &operation_index) != 0)
            continue;

        pthread_mutex_lock(&restaurant->state_mutex);
        operation = restaurant->operations[operation_index];
        dish_name = restaurant->config->menu[
            restaurant->orders[operation.order_index]
                .dishes[operation.dish_index].menu_index].name;
        pthread_mutex_unlock(&restaurant->state_mutex);

        if (sem_trywait(&restaurant->equipment[operation.equipment]) != 0) {
            if (errno != EAGAIN) {
                atomic_store(&restaurant->failed, 1);
                break;
            }
            logger_event(&restaurant->logger, model_time(restaurant),
                         "Cook %u waits for %s for operation %u of dish '%s'",
                         cook_id, config_equipment_name(operation.equipment),
                         operation.step_index + 1, dish_name);
            if (wait_sem_interruptible(
                    restaurant, &restaurant->equipment[operation.equipment]) != 0)
                break;
        }

        pthread_mutex_lock(&restaurant->state_mutex);
        restaurant->operations[operation_index].state = OP_RUNNING;
        restaurant->operations[operation_index].cook_id = cook_id;
        ++restaurant->stats.operations_started;
        restaurant->stats.total_queue_wait_ms +=
            monotonic_ms() - restaurant->operations[operation_index].queued_ms;
        pthread_mutex_unlock(&restaurant->state_mutex);

        logger_event(&restaurant->logger, model_time(restaurant),
                     "Cook %u acquired %s for operation %u of dish '%s' "
                     "for order %u",
                     cook_id, config_equipment_name(operation.equipment),
                     operation.step_index + 1, dish_name,
                     operation.order_index + 1);
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Cook %u started operation %u of dish '%s' for order %u "
                     "(duration=%u)",
                     cook_id, operation.step_index + 1, dish_name,
                     operation.order_index + 1, operation.duration);

        if (sleep_units(restaurant, operation.duration) != 0) {
            sem_post(&restaurant->equipment[operation.equipment]);
            break;
        }

        sem_post(&restaurant->equipment[operation.equipment]);

        pthread_mutex_lock(&restaurant->state_mutex);
        {
            RuntimeOperation *runtime = &restaurant->operations[operation_index];
            RuntimeDish *dish = &restaurant->orders[runtime->order_index]
                                     .dishes[runtime->dish_index];

            runtime->state = OP_DONE;
            ++dish->completed_operations;
            ++restaurant->stats.operations_completed;
            restaurant->stats.cook_busy_units += runtime->duration;
            restaurant->stats.equipment_busy_units[runtime->equipment] +=
                runtime->duration;

            final_operation =
                dish->completed_operations == dish->operation_count;

            if (!final_operation) {
                unsigned next_id = dish->operation_ids[dish->completed_operations];
                RuntimeOperation *next = &restaurant->operations[next_id];
                next->state = OP_READY;
                next->ready_sequence = ++restaurant->next_ready_sequence;
            } else {
                ++restaurant->stats.dishes_ready;
            }
        }
        pthread_mutex_unlock(&restaurant->state_mutex);

        logger_event(&restaurant->logger, model_time(restaurant),
                     "Cook %u finished operation %u of dish '%s' for order %u "
                     "and released %s",
                     cook_id, operation.step_index + 1, dish_name,
                     operation.order_index + 1,
                     config_equipment_name(operation.equipment));

        if (final_operation) {
            ReadyDish ready_item;

            logger_event(&restaurant->logger, model_time(restaurant),
                         "Dish '%s' is ready for order %u",
                         dish_name, operation.order_index + 1);

            if (sem_trywait(&restaurant->ready_slots) != 0) {
                if (errno != EAGAIN) {
                    atomic_store(&restaurant->failed, 1);
                    break;
                }
                logger_event(&restaurant->logger, model_time(restaurant),
                             "Ready area is full; Cook %u waits with dish '%s' "
                             "for order %u",
                             cook_id, dish_name, operation.order_index + 1);
                if (wait_sem_interruptible(restaurant,
                                           &restaurant->ready_slots) != 0)
                    break;
            }

            ready_item.order_index = operation.order_index;
            ready_item.dish_index = operation.dish_index;

            pthread_mutex_lock(&restaurant->state_mutex);
            restaurant->ready_queue[restaurant->ready_count++] = ready_item;
            pthread_mutex_unlock(&restaurant->state_mutex);

            logger_event(&restaurant->logger, model_time(restaurant),
                         "Cook %u placed dish '%s' for order %u in ready area",
                         cook_id, dish_name, operation.order_index + 1);
            sem_post(&restaurant->coordinator_events);
        } else {
            sem_post(&restaurant->coordinator_events);
        }
    }

    logger_event(&restaurant->logger, model_time(restaurant),
                 "Cook %u stopped", cook_id);
    return NULL;
}

static int find_waiting_order_locked(Restaurant *restaurant)
{
    int chosen = -1;
    unsigned i;

    for (i = 0; i < restaurant->config->visitor_count; ++i) {
        RuntimeOrder *order = &restaurant->orders[i];
        if (order->state != ORDER_WAITING_WAITER)
            continue;
        if (chosen < 0 || order->arrival_sequence <
                              restaurant->orders[(unsigned)chosen].arrival_sequence)
            chosen = (int)i;
    }
    return chosen;
}

/* Официант обслуживает один активный заказ: принимает его и затем выдает. */
static void *waiter_thread_main(void *argument)
{
    ThreadArg *arg = (ThreadArg *)argument;
    Restaurant *restaurant = arg->restaurant;
    unsigned waiter_id = arg->index + 1;

    logger_event(&restaurant->logger, model_time(restaurant),
                 "Waiter %u started shift", waiter_id);

    while (!stop_requested(restaurant)) {
        int order_index;

        if (wait_sem_interruptible(restaurant, &restaurant->visitor_waiting) != 0)
            break;
        if (stop_requested(restaurant))
            break;

        pthread_mutex_lock(&restaurant->state_mutex);
        order_index = find_waiting_order_locked(restaurant);
        if (order_index >= 0) {
            RuntimeOrder *order = &restaurant->orders[(unsigned)order_index];
            order->state = ORDER_SUBMITTED;
            order->waiter_id = waiter_id;
            ++restaurant->stats.orders_accepted;
        }
        pthread_mutex_unlock(&restaurant->state_mutex);

        if (order_index < 0)
            continue;

        logger_event(&restaurant->logger, model_time(restaurant),
                     "Waiter %u accepted order %u from visitor %u",
                     waiter_id, (unsigned)order_index + 1,
                     (unsigned)order_index + 1);
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Waiter %u passed order %u to kitchen coordinator",
                     waiter_id, (unsigned)order_index + 1);
        sem_post(&restaurant->coordinator_events);

        if (wait_sem_interruptible(
                restaurant, &restaurant->order_ready[(unsigned)order_index]) != 0)
            break;
        if (stop_requested(restaurant))
            break;

        pthread_mutex_lock(&restaurant->state_mutex);
        if (restaurant->orders[(unsigned)order_index].state == ORDER_ASSEMBLED) {
            restaurant->orders[(unsigned)order_index].state = ORDER_DELIVERED;
            ++restaurant->stats.orders_delivered;
            pthread_mutex_unlock(&restaurant->state_mutex);

            logger_event(&restaurant->logger, model_time(restaurant),
                         "Waiter %u delivered order %u to visitor %u",
                         waiter_id, (unsigned)order_index + 1,
                         (unsigned)order_index + 1);
            sem_post(&restaurant->visitor_done[(unsigned)order_index]);
        } else if (restaurant->orders[(unsigned)order_index].state ==
                   ORDER_CANCELED) {
            pthread_mutex_unlock(&restaurant->state_mutex);
            logger_event(&restaurant->logger, model_time(restaurant),
                         "Waiter %u informed visitor %u that order %u was canceled",
                         waiter_id, (unsigned)order_index + 1,
                         (unsigned)order_index + 1);
            sem_post(&restaurant->visitor_done[(unsigned)order_index]);
        } else {
            pthread_mutex_unlock(&restaurant->state_mutex);
            atomic_store(&restaurant->failed, 1);
            break;
        }
    }

    logger_event(&restaurant->logger, model_time(restaurant),
                 "Waiter %u stopped", waiter_id);
    return NULL;
}

/* Поток посетителя отвечает за приход, ожидание официанта и получение заказа. */
static void *visitor_thread_main(void *argument)
{
    ThreadArg *arg = (ThreadArg *)argument;
    Restaurant *restaurant = arg->restaurant;
    unsigned visitor_index = arg->index;
    RuntimeOrder *order = &restaurant->orders[visitor_index];
    uint64_t arrival_time = arrival_time_for(restaurant->config, visitor_index);
    char dishes[512];
    OrderState final_state;
    int reception_closed = 0;

    if (sleep_units(restaurant, arrival_time) != 0)
        return NULL;
    if (stop_requested(restaurant))
        return NULL;

    pthread_mutex_lock(&restaurant->state_mutex);
    order->state = ORDER_WAITING_WAITER;
    order->arrival_sequence = ++restaurant->next_arrival_sequence;
    ++restaurant->stats.visitors_arrived;
    if (restaurant->stats.visitors_arrived == restaurant->config->visitor_count)
        reception_closed = 1;
    pthread_mutex_unlock(&restaurant->state_mutex);

    format_order(restaurant, order, dishes, sizeof(dishes));
    logger_event(&restaurant->logger, model_time(restaurant),
                 "Visitor %u arrived and formed order %u: [%s]",
                 order->visitor_id, order->visitor_id, dishes);
    if (reception_closed) {
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Order reception is closed: all planned visitors have arrived");
    }
    sem_post(&restaurant->visitor_waiting);

    if (wait_sem_interruptible(restaurant,
                               &restaurant->visitor_done[visitor_index]) != 0)
        return NULL;
    if (stop_requested(restaurant))
        return NULL;

    pthread_mutex_lock(&restaurant->state_mutex);
    final_state = order->state;
    pthread_mutex_unlock(&restaurant->state_mutex);

    if (final_state == ORDER_DELIVERED) {
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Visitor %u received order %u and left restaurant",
                     order->visitor_id, order->visitor_id);
    } else if (final_state == ORDER_CANCELED) {
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Visitor %u left restaurant without order because it was canceled",
                     order->visitor_id);
    }

    sem_post(&restaurant->visitor_finished);
    return NULL;
}

static void print_summary(Restaurant *restaurant,
                          const char *termination)
{
    uint64_t elapsed_ms = monotonic_ms() - restaurant->start_ms;
    double elapsed_units = (double)elapsed_ms /
                           (double)restaurant->options->time_unit_ms;
    double queue_wait = 0.0;
    double cook_utilization = 0.0;
    unsigned i;

    if (elapsed_units < 1.0)
        elapsed_units = 1.0;

    if (restaurant->stats.operations_started > 0) {
        queue_wait = (double)restaurant->stats.total_queue_wait_ms /
                     (double)restaurant->options->time_unit_ms /
                     (double)restaurant->stats.operations_started;
    }
    if (restaurant->config->cook_count > 0) {
        cook_utilization = 100.0 * (double)restaurant->stats.cook_busy_units /
                           ((double)restaurant->config->cook_count * elapsed_units);
    }

    logger_line(&restaurant->logger,
                "\n================ FINAL STATISTICS ================\n");
    logger_line(&restaurant->logger, "Termination: %s\n", termination);
    logger_line(&restaurant->logger, "Elapsed real time: %llu ms\n",
                (unsigned long long)elapsed_ms);
    logger_line(&restaurant->logger, "Elapsed model time: %.2f units\n",
                elapsed_units);
    logger_line(&restaurant->logger, "Visitors arrived: %u / %u\n",
                restaurant->stats.visitors_arrived,
                restaurant->config->visitor_count);
    logger_line(&restaurant->logger, "Orders accepted: %u\n",
                restaurant->stats.orders_accepted);
    logger_line(&restaurant->logger, "Orders delivered: %u\n",
                restaurant->stats.orders_delivered);
    logger_line(&restaurant->logger, "Orders canceled as impossible: %u\n",
                restaurant->stats.orders_canceled);
    logger_line(&restaurant->logger, "Dishes created / ready: %u / %u\n",
                restaurant->stats.dishes_created,
                restaurant->stats.dishes_ready);
    logger_line(&restaurant->logger,
                "Operations created / started / completed: %u / %u / %u\n",
                restaurant->stats.operations_created,
                restaurant->stats.operations_started,
                restaurant->stats.operations_completed);
    logger_line(&restaurant->logger,
                "Average kitchen-queue wait: %.2f model units\n", queue_wait);
    logger_line(&restaurant->logger, "Cook utilization: %.2f%%\n",
                cook_utilization);

    for (i = 0; i < EQUIPMENT_TYPES; ++i) {
        double utilization = 0.0;
        if (restaurant->config->equipment_count[i] > 0) {
            utilization =
                100.0 * (double)restaurant->stats.equipment_busy_units[i] /
                ((double)restaurant->config->equipment_count[i] * elapsed_units);
        }
        logger_line(&restaurant->logger, "%-5s utilization: %.2f%% (units: %u)\n",
                    config_equipment_name((EquipmentType)i), utilization,
                    restaurant->config->equipment_count[i]);
    }
    logger_line(&restaurant->logger,
                "==================================================\n");
}

static void wake_all(Restaurant *restaurant)
{
    unsigned i;

    for (i = 0; i < restaurant->config->waiter_count + 1; ++i)
        sem_post(&restaurant->visitor_waiting);
    for (i = 0; i < restaurant->config->cook_count + 1; ++i)
        sem_post(&restaurant->kitchen_items);
    sem_post(&restaurant->coordinator_events);

    for (i = 0; i < restaurant->config->visitor_count; ++i) {
        sem_post(&restaurant->order_ready[i]);
        sem_post(&restaurant->visitor_done[i]);
    }
}

static void destroy_restaurant(Restaurant *restaurant)
{
    unsigned i;

    if (restaurant == NULL)
        return;

    for (i = 0; i < restaurant->order_semaphores_initialized; ++i) {
        sem_destroy(&restaurant->order_ready[i]);
        sem_destroy(&restaurant->visitor_done[i]);
    }
    for (i = 0; i < restaurant->equipment_semaphores_initialized; ++i)
        sem_destroy(&restaurant->equipment[i]);

    if (restaurant->base_semaphores_initialized >= 1)
        sem_destroy(&restaurant->visitor_waiting);
    if (restaurant->base_semaphores_initialized >= 2)
        sem_destroy(&restaurant->coordinator_events);
    if (restaurant->base_semaphores_initialized >= 3)
        sem_destroy(&restaurant->kitchen_items);
    if (restaurant->base_semaphores_initialized >= 4)
        sem_destroy(&restaurant->kitchen_slots);
    if (restaurant->base_semaphores_initialized >= 5)
        sem_destroy(&restaurant->ready_slots);
    if (restaurant->base_semaphores_initialized >= 6)
        sem_destroy(&restaurant->visitor_finished);
    if (restaurant->mutex_initialized)
        pthread_mutex_destroy(&restaurant->state_mutex);
    if (restaurant->logger_initialized)
        logger_close(&restaurant->logger);

    free(restaurant->order_ready);
    free(restaurant->visitor_done);
    free(restaurant->orders);
    free(restaurant->operations);
    free(restaurant->kitchen_queue);
    free(restaurant->ready_queue);
    free(restaurant->visitor_threads);
    free(restaurant->waiter_threads);
    free(restaurant->cook_threads);
    free(restaurant->visitor_args);
    free(restaurant->waiter_args);
    free(restaurant->cook_args);
    free(restaurant);
}

/* Создание общей модели и всех примитивов синхронизации. */
static Restaurant *create_restaurant(const Config *config,
                                     const SimulationOptions *options)
{
    Restaurant *restaurant;
    unsigned kitchen_capacity;
    unsigned ready_capacity;
    unsigned i;

    restaurant = calloc(1, sizeof(*restaurant));
    if (restaurant == NULL)
        return NULL;

    restaurant->config = config;
    restaurant->options = options;
    atomic_init(&restaurant->shutdown, 0);
    atomic_init(&restaurant->failed, 0);

    restaurant->orders = calloc(config->visitor_count, sizeof(*restaurant->orders));
    restaurant->operations = calloc(MAX_OPERATIONS, sizeof(*restaurant->operations));
    restaurant->kitchen_queue = calloc(MAX_OPERATIONS, sizeof(*restaurant->kitchen_queue));
    restaurant->ready_queue = calloc(MAX_READY_ITEMS, sizeof(*restaurant->ready_queue));
    restaurant->order_ready = calloc(config->visitor_count, sizeof(*restaurant->order_ready));
    restaurant->visitor_done = calloc(config->visitor_count, sizeof(*restaurant->visitor_done));
    restaurant->visitor_threads = calloc(config->visitor_count,
                                         sizeof(*restaurant->visitor_threads));
    restaurant->waiter_threads = calloc(config->waiter_count,
                                        sizeof(*restaurant->waiter_threads));
    restaurant->cook_threads = calloc(config->cook_count,
                                      sizeof(*restaurant->cook_threads));
    restaurant->visitor_args = calloc(config->visitor_count,
                                      sizeof(*restaurant->visitor_args));
    restaurant->waiter_args = calloc(config->waiter_count,
                                     sizeof(*restaurant->waiter_args));
    restaurant->cook_args = calloc(config->cook_count,
                                   sizeof(*restaurant->cook_args));

    if (restaurant->orders == NULL || restaurant->operations == NULL ||
        restaurant->kitchen_queue == NULL || restaurant->ready_queue == NULL ||
        restaurant->order_ready == NULL || restaurant->visitor_done == NULL ||
        restaurant->visitor_threads == NULL || restaurant->waiter_threads == NULL ||
        restaurant->cook_threads == NULL || restaurant->visitor_args == NULL ||
        restaurant->waiter_args == NULL || restaurant->cook_args == NULL) {
        destroy_restaurant(restaurant);
        return NULL;
    }

    if (logger_open(&restaurant->logger, options->log_path) != 0) {
        destroy_restaurant(restaurant);
        return NULL;
    }
    restaurant->logger_initialized = 1;

    if (pthread_mutex_init(&restaurant->state_mutex, NULL) != 0) {
        destroy_restaurant(restaurant);
        return NULL;
    }
    restaurant->mutex_initialized = 1;

    kitchen_capacity = config->kitchen_queue_capacity == 0
                           ? MAX_OPERATIONS
                           : config->kitchen_queue_capacity;
    ready_capacity = config->ready_queue_capacity == 0
                         ? MAX_READY_ITEMS
                         : config->ready_queue_capacity;

    if (sem_init(&restaurant->visitor_waiting, 0, 0) != 0) {
        perror("sem_init visitor_waiting");
        destroy_restaurant(restaurant);
        return NULL;
    }
    restaurant->base_semaphores_initialized = 1;
    if (sem_init(&restaurant->coordinator_events, 0, 0) != 0) {
        perror("sem_init coordinator_events");
        destroy_restaurant(restaurant);
        return NULL;
    }
    restaurant->base_semaphores_initialized = 2;
    if (sem_init(&restaurant->kitchen_items, 0, 0) != 0) {
        perror("sem_init kitchen_items");
        destroy_restaurant(restaurant);
        return NULL;
    }
    restaurant->base_semaphores_initialized = 3;
    if (sem_init(&restaurant->kitchen_slots, 0, kitchen_capacity) != 0) {
        perror("sem_init kitchen_slots");
        destroy_restaurant(restaurant);
        return NULL;
    }
    restaurant->base_semaphores_initialized = 4;
    if (sem_init(&restaurant->ready_slots, 0, ready_capacity) != 0) {
        perror("sem_init ready_slots");
        destroy_restaurant(restaurant);
        return NULL;
    }
    restaurant->base_semaphores_initialized = 5;
    if (sem_init(&restaurant->visitor_finished, 0, 0) != 0) {
        perror("sem_init visitor_finished");
        destroy_restaurant(restaurant);
        return NULL;
    }
    restaurant->base_semaphores_initialized = 6;

    for (i = 0; i < EQUIPMENT_TYPES; ++i) {
        if (sem_init(&restaurant->equipment[i], 0,
                     config->equipment_count[i]) != 0) {
            perror("sem_init equipment");
            destroy_restaurant(restaurant);
            return NULL;
        }
        ++restaurant->equipment_semaphores_initialized;
    }

    for (i = 0; i < config->visitor_count; ++i) {
        if (sem_init(&restaurant->order_ready[i], 0, 0) != 0) {
            perror("sem_init order_ready");
            destroy_restaurant(restaurant);
            return NULL;
        }
        if (sem_init(&restaurant->visitor_done[i], 0, 0) != 0) {
            perror("sem_init visitor_done");
            sem_destroy(&restaurant->order_ready[i]);
            destroy_restaurant(restaurant);
            return NULL;
        }
        ++restaurant->order_semaphores_initialized;
    }

    prepare_orders(restaurant);
    return restaurant;
}

static int create_threads(Restaurant *restaurant)
{
    unsigned i;

    if (pthread_create(&restaurant->coordinator_thread, NULL,
                       coordinator_thread_main, restaurant) != 0)
        return -1;
    restaurant->coordinator_created = 1;

    for (i = 0; i < restaurant->config->waiter_count; ++i) {
        restaurant->waiter_args[i].restaurant = restaurant;
        restaurant->waiter_args[i].index = i;
        if (pthread_create(&restaurant->waiter_threads[i], NULL,
                           waiter_thread_main,
                           &restaurant->waiter_args[i]) != 0)
            return -1;
        ++restaurant->waiters_created;
    }

    for (i = 0; i < restaurant->config->cook_count; ++i) {
        restaurant->cook_args[i].restaurant = restaurant;
        restaurant->cook_args[i].index = i;
        if (pthread_create(&restaurant->cook_threads[i], NULL,
                           cook_thread_main,
                           &restaurant->cook_args[i]) != 0)
            return -1;
        ++restaurant->cooks_created;
    }

    for (i = 0; i < restaurant->config->visitor_count; ++i) {
        restaurant->visitor_args[i].restaurant = restaurant;
        restaurant->visitor_args[i].index = i;
        if (pthread_create(&restaurant->visitor_threads[i], NULL,
                           visitor_thread_main,
                           &restaurant->visitor_args[i]) != 0)
            return -1;
        ++restaurant->visitors_created;
    }

    return 0;
}

static void join_threads(Restaurant *restaurant)
{
    unsigned i;

    for (i = 0; i < restaurant->visitors_created; ++i)
        pthread_join(restaurant->visitor_threads[i], NULL);
    for (i = 0; i < restaurant->waiters_created; ++i)
        pthread_join(restaurant->waiter_threads[i], NULL);
    for (i = 0; i < restaurant->cooks_created; ++i)
        pthread_join(restaurant->cook_threads[i], NULL);
    if (restaurant->coordinator_created)
        pthread_join(restaurant->coordinator_thread, NULL);
}

SimulationResult simulation_run(const Config *config,
                                const SimulationOptions *options)
{
    Restaurant *restaurant;
    SimulationResult result = SIMULATION_OK;
    unsigned visitors_finished = 0;

    if (config == NULL || options == NULL || options->time_unit_ms == 0 ||
        config_validate(config) != 0)
        return SIMULATION_ERROR;

    restaurant = create_restaurant(config, options);
    if (restaurant == NULL)
        return SIMULATION_ERROR;

    restaurant->start_ms = monotonic_ms();
    logger_event(&restaurant->logger, 0,
                 "Restaurant opened; multithreaded simulation started");
    logger_event(&restaurant->logger, 0,
                 "Threads: visitors=%u, waiters=%u, cooks=%u, coordinator=1; "
                 "priority=%s, time_unit=%u ms",
                 config->visitor_count, config->waiter_count, config->cook_count,
                 config->priority == PRIORITY_FIFO ? "FIFO" : "LIFO",
                 options->time_unit_ms);

    if (create_threads(restaurant) != 0) {
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Failed to create all required threads");
        result = SIMULATION_ERROR;
    } else {
        while (visitors_finished < config->visitor_count) {
            if (wait_sem_interruptible(restaurant,
                                       &restaurant->visitor_finished) != 0) {
                if (options->stop_flag != NULL && *options->stop_flag != 0)
                    result = SIMULATION_INTERRUPTED;
                else if (atomic_load(&restaurant->failed) != 0)
                    result = SIMULATION_ERROR;
                else
                    result = SIMULATION_ERROR;
                break;
            }
            ++visitors_finished;

            if (atomic_load(&restaurant->failed) != 0) {
                result = SIMULATION_ERROR;
                break;
            }
        }
    }

    if (result == SIMULATION_INTERRUPTED) {
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Interrupt received; restaurant is stopping safely");
    } else if (result == SIMULATION_OK) {
        logger_event(&restaurant->logger, model_time(restaurant),
                     "Restaurant closed: delivered=%u, canceled=%u, total=%u",
                     restaurant->stats.orders_delivered,
                     restaurant->stats.orders_canceled,
                     config->visitor_count);
    }

    atomic_store(&restaurant->shutdown, 1);
    wake_all(restaurant);
    join_threads(restaurant);

    if (atomic_load(&restaurant->failed) != 0 && result == SIMULATION_OK)
        result = SIMULATION_ERROR;

    if (result == SIMULATION_OK)
        print_summary(restaurant, "normal");
    else if (result == SIMULATION_INTERRUPTED)
        print_summary(restaurant, "interrupted");
    else
        print_summary(restaurant, "error");

    destroy_restaurant(restaurant);
    return result;
}
