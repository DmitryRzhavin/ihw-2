#define _POSIX_C_SOURCE 200809L

#include "config.h"
#include "simulation.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    OPT_VISITORS,
    OPT_WAITERS,
    OPT_COOKS,
    OPT_ARRIVAL_INTERVAL,
    OPT_DISHES_PER_ORDER,
    OPT_KITCHEN_CAP,
    OPT_READY_CAP,
    OPT_STOVES,
    OPT_OVENS,
    OPT_GRILLS,
    OPT_TABLES,
    OPT_SEED,
    OPT_COUNT
} NumericOption;

static volatile sig_atomic_t g_stop_signal = 0;

static const char *numeric_option_names[OPT_COUNT] = {
    "--visitors",
    "--waiters",
    "--cooks",
    "--arrival-interval",
    "--dishes-per-order",
    "--kitchen-cap",
    "--ready-cap",
    "--stoves",
    "--ovens",
    "--grills",
    "--tables",
    "--seed"
};

static void handle_stop_signal(int signal_number)
{
    g_stop_signal = signal_number;
}

static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0 ||
        sigaction(SIGTERM, &action, NULL) != 0) {
        perror("sigaction");
        return -1;
    }
    return 0;
}

static int parse_unsigned(const char *text, unsigned *result)
{
    char *end = NULL;
    unsigned long value;

    if (text == NULL || *text == '\0' || *text == '-')
        return -1;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT_MAX)
        return -1;

    *result = (unsigned)value;
    return 0;
}

static void print_usage(const char *program)
{
    printf(
        "Restaurant kitchen multithreaded simulation, variant 32\n"
        "Usage: %s [--config FILE] [options]\n\n"
        "Without --config, configuration is entered interactively.\n\n"
        "General options:\n"
        "  --config FILE          read configuration from FILE\n"
        "  --log FILE             write the event log to FILE\n"
        "  --no-log               disable file logging\n"
        "  --unit-ms N            real milliseconds per model time unit\n"
        "  --priority fifo|lifo   override kitchen priority rule\n"
        "  --help                  show this help\n\n"
        "Numeric overrides:\n"
        "  --visitors N --waiters N --cooks N\n"
        "  --arrival-interval N --dishes-per-order N\n"
        "  --kitchen-cap N --ready-cap N\n"
        "  --stoves N --ovens N --grills N --tables N\n"
        "  --seed N\n\n"
        "Press Ctrl+C for graceful shutdown.\n",
        program);
}

static int find_numeric_option(const char *name)
{
    unsigned i;

    for (i = 0; i < OPT_COUNT; ++i) {
        if (strcmp(name, numeric_option_names[i]) == 0)
            return (int)i;
    }
    return -1;
}

static void apply_numeric_option(Config *config,
                                 NumericOption option,
                                 unsigned value)
{
    switch (option) {
    case OPT_VISITORS:
        config->visitor_count = value;
        break;
    case OPT_WAITERS:
        config->waiter_count = value;
        break;
    case OPT_COOKS:
        config->cook_count = value;
        break;
    case OPT_ARRIVAL_INTERVAL:
        config->arrival_interval = value;
        config->arrival_gap_count = 0;
        break;
    case OPT_DISHES_PER_ORDER:
        config->dishes_per_order = value;
        break;
    case OPT_KITCHEN_CAP:
        config->kitchen_queue_capacity = value;
        break;
    case OPT_READY_CAP:
        config->ready_queue_capacity = value;
        break;
    case OPT_STOVES:
        config->equipment_count[EQUIPMENT_STOVE] = value;
        break;
    case OPT_OVENS:
        config->equipment_count[EQUIPMENT_OVEN] = value;
        break;
    case OPT_GRILLS:
        config->equipment_count[EQUIPMENT_GRILL] = value;
        break;
    case OPT_TABLES:
        config->equipment_count[EQUIPMENT_TABLE] = value;
        break;
    case OPT_SEED:
        config->seed = (uint32_t)value;
        break;
    case OPT_COUNT:
        break;
    }
}

int main(int argc, char **argv)
{
    const char *config_path = NULL;
    const char *log_path = "restaurant.log";
    unsigned time_unit_ms = 100;
    unsigned overrides[OPT_COUNT] = {0};
    int has_override[OPT_COUNT] = {0};
    int disable_log = 0;
    int priority_override = -1;
    Config config;
    SimulationOptions options;
    SimulationResult result;
    int i;

    for (i = 1; i < argc; ++i) {
        int option;

        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }

        if (strcmp(argv[i], "--config") == 0 ||
            strcmp(argv[i], "--log") == 0 ||
            strcmp(argv[i], "--unit-ms") == 0 ||
            strcmp(argv[i], "--priority") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", argv[i]);
                return 2;
            }

            if (strcmp(argv[i], "--config") == 0) {
                config_path = argv[++i];
            } else if (strcmp(argv[i], "--log") == 0) {
                log_path = argv[++i];
            } else if (strcmp(argv[i], "--unit-ms") == 0) {
                if (parse_unsigned(argv[++i], &time_unit_ms) != 0 ||
                    time_unit_ms == 0) {
                    fprintf(stderr, "Invalid --unit-ms value\n");
                    return 2;
                }
            } else {
                const char *value = argv[++i];
                if (strcmp(value, "fifo") == 0)
                    priority_override = PRIORITY_FIFO;
                else if (strcmp(value, "lifo") == 0)
                    priority_override = PRIORITY_LIFO;
                else {
                    fprintf(stderr, "Priority must be fifo or lifo\n");
                    return 2;
                }
            }
            continue;
        }

        if (strcmp(argv[i], "--no-log") == 0) {
            disable_log = 1;
            continue;
        }

        option = find_numeric_option(argv[i]);
        if (option < 0) {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
        if (i + 1 >= argc ||
            parse_unsigned(argv[i + 1], &overrides[option]) != 0) {
            fprintf(stderr, "Invalid value for %s\n", argv[i]);
            return 2;
        }
        has_override[option] = 1;
        ++i;
    }

    if (config_path != NULL) {
        if (config_load_file(config_path, &config) != 0)
            return 1;
    } else if (config_read_interactive(&config) != 0) {
        return 1;
    }

    for (i = 0; i < OPT_COUNT; ++i) {
        if (has_override[i])
            apply_numeric_option(&config, (NumericOption)i, overrides[i]);
    }
    if (priority_override >= 0)
        config.priority = (OrderPriority)priority_override;

    if (config_validate(&config) != 0)
        return 1;
    if (install_signal_handlers() != 0)
        return 1;

    options.log_path = disable_log ? NULL : log_path;
    options.time_unit_ms = time_unit_ms;
    options.stop_flag = &g_stop_signal;

    result = simulation_run(&config, &options);
    if (result == SIMULATION_OK)
        return 0;
    if (result == SIMULATION_INTERRUPTED)
        return 130;
    return 1;
}
