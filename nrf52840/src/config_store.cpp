#include "config_store.h"
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <math.h>
#include <string.h>

using namespace Adafruit_LittleFS_Namespace;

#define CONFIG_PATH  "/treadlink.cfg"
#define CONFIG_MAGIC 0x544C4B32 // "TLK2"

void config_store_init(void)
{
    InternalFS.begin();
}

void config_store_defaults(treadlink_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->magic = CONFIG_MAGIC;
    config->auto_connect = true;
    config->cadence_factor = 1.4f;
    config->cadence_offset = 120.0f;
    strcpy(config->ble_pin, "123456");
}

void config_store_load(treadlink_config_t *config)
{
    config_store_defaults(config);

    File f(InternalFS);
    if (!f.open(CONFIG_PATH, FILE_O_READ)) return;

    // A shorter file is an older version: fields it lacks keep their defaults
    treadlink_config_t tmp = *config;
    int n = f.read((uint8_t *)&tmp, sizeof(tmp));
    f.close();
    if (n < (int)sizeof(tmp.magic) || tmp.magic != CONFIG_MAGIC) return;

    *config = tmp;
    config->treadmill_name[sizeof(config->treadmill_name) - 1] = '\0';
    if (!isfinite(config->cadence_factor) || config->cadence_factor < 0.0f || config->cadence_factor > 10.0f)
        config->cadence_factor = 1.4f;
    if (!isfinite(config->cadence_offset) || config->cadence_offset < 0.0f || config->cadence_offset > 250.0f)
        config->cadence_offset = 120.0f;
    config->ble_pin[sizeof(config->ble_pin) - 1] = '\0';
    if (strlen(config->ble_pin) != 6) strcpy(config->ble_pin, "123456");
}

bool config_store_save(const treadlink_config_t *config)
{
    InternalFS.remove(CONFIG_PATH);
    File f(InternalFS);
    if (!f.open(CONFIG_PATH, FILE_O_WRITE)) return false;
    bool ok = f.write((const uint8_t *)config, sizeof(*config)) == sizeof(*config);
    f.close();
    return ok;
}
