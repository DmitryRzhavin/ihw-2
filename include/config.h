#ifndef CONFIG_H
#define CONFIG_H

#include "model.h"

int config_load_file(const char *path, Config *config);
int config_read_interactive(Config *config);
int config_validate(const Config *config);
const char *config_equipment_name(EquipmentType equipment);

#endif
