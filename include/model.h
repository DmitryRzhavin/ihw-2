#ifndef MODEL_H
#define MODEL_H

#include <stdint.h>

#define MAX_DISHES 12
#define MAX_STEPS_PER_DISH 10
#define MAX_NAME_LENGTH 48
#define MAX_VISITORS 100
#define MAX_DISHES_PER_ORDER 6
#define EQUIPMENT_TYPES 4

typedef enum {
    EQUIPMENT_STOVE = 0,
    EQUIPMENT_OVEN,
    EQUIPMENT_GRILL,
    EQUIPMENT_TABLE
} EquipmentType;

typedef enum {
    PRIORITY_FIFO = 0,
    PRIORITY_LIFO
} OrderPriority;

typedef struct {
    EquipmentType equipment;
    unsigned duration;
} OperationSpec;

typedef struct {
    char name[MAX_NAME_LENGTH];
    unsigned operation_count;
    OperationSpec operations[MAX_STEPS_PER_DISH];
} DishSpec;

typedef struct {
    unsigned visitor_count;
    unsigned waiter_count;
    unsigned cook_count;
    unsigned equipment_count[EQUIPMENT_TYPES];

    /*
     * Если arrival_gap_count == 0, посетитель 1 приходит в t=0,
     * остальные — через arrival_interval единиц модельного времени.
     * Иначе arrival_gaps[i] задает интервал от предыдущего прихода
     * (для первого посетителя — от открытия ресторана).
     */
    unsigned arrival_interval;
    unsigned arrival_gap_count;
    unsigned arrival_gaps[MAX_VISITORS];

    /* Ноль означает неограниченную вместимость. */
    unsigned kitchen_queue_capacity;
    unsigned ready_queue_capacity;
    OrderPriority priority;

    /* Используется, если в конфигурации нет строк order=. */
    unsigned dishes_per_order;
    uint32_t seed;

    unsigned menu_size;
    DishSpec menu[MAX_DISHES];

    /* Индексы блюд меню для явно заданных заказов. */
    unsigned order_count;
    unsigned order_dish_count[MAX_VISITORS];
    unsigned order_dishes[MAX_VISITORS][MAX_DISHES_PER_ORDER];
} Config;

#endif
