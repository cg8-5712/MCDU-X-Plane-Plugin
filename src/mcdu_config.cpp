#include "mcdu_config.h"

MCDUConfig& MCDUConfigGet() {
    static MCDUConfig cfg;
    return cfg;
}
