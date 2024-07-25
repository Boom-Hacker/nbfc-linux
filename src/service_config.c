#include "service_config.h"

#include "nbfc.h"
#include "error.h"
#include "memory.h"
#include "model_config.h"
#include "nxjson_utils.h"
#include "reverse_nxjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ServiceConfig service_config = {0};

Error* ServiceConfig_Init(const char* file) {
  char buf[NBFC_MAX_FILE_SIZE];
  const nx_json* js = NULL;
  Error* e = nx_json_parse_file(&js, buf, sizeof(buf), file);
  if (e)
    return e;

  e = ServiceConfig_FromJson(&service_config, js);
  nx_json_free(js);
  if (e)
    return e;

  e = ServiceConfig_ValidateFields(&service_config);
  if (e)
    return e;

  for_each_array(float*, f, service_config.TargetFanSpeeds) {
    if (*f > 100.0f) {
      e = err_string(0, "TargetFanSpeeds: value cannot be greater than 100.0");
      e = err_string(e, file);
      e_warn();
      *f = 100.0f;
    }

    if (*f < 0.0f && *f != -1.0f) {
      e = err_string(0, "TargetFanSpeeds: Please use `-1' for selecting auto mode");
      e = err_string(e, file);
      e_warn();
      *f = -1.0f;
    }
  }

  for_each_array(FanTemperatureSourceConfig*, ftsc, service_config.FanTemperatureSources) {
    e = FanTemperatureSourceConfig_ValidateFields(ftsc);
    if (e)
      return e;
  }

  return err_success();
}

Error* ServiceConfig_Write(const char* file) {
  nx_json root = {0};
  nx_json *o = create_json_object(NULL, &root);

  if (service_config.SelectedConfigId != NULL)
    create_json_string("SelectedConfigId", o, service_config.SelectedConfigId);

  if (service_config.EmbeddedControllerType != EmbeddedControllerType_Unset)
    create_json_string("EmbeddedControllerType", o, EmbeddedControllerType_ToString(service_config.EmbeddedControllerType));

  if (service_config.TargetFanSpeeds.size) {
    nx_json* fanspeeds = create_json_array("TargetFanSpeeds", o);

    for_each_array(float*, f, service_config.TargetFanSpeeds)
      create_json_double(NULL, fanspeeds, *f);
  }

  if (service_config.FanTemperatureSources.size) {
    nx_json* fan_temperature_sources = create_json_array("FanTemperatureSources", o);

    for_each_array(FanTemperatureSourceConfig*, ftsc, service_config.FanTemperatureSources) {
      nx_json* fan_temperature_source = create_json_object(NULL, fan_temperature_sources);

      create_json_integer("FanIndex", fan_temperature_source, ftsc->FanIndex);
      create_json_string("TemperatureAlgorithmType", fan_temperature_source, TemperatureAlgorithmType_ToString(ftsc->TemperatureAlgorithmType));
      if (ftsc->Sensors.size) {
        nx_json* sensors = create_json_array("Sensors", fan_temperature_source);
        for_each_array(const char**, sensor, ftsc->Sensors) {
          create_json_string(NULL, sensors, *sensor);
        }
      }
    }
  }

  char buf[NBFC_MAX_FILE_SIZE];
  StringBuf s = { buf, 0, sizeof(buf) };
  buf[0] = '\0';

  nx_json_to_string(o, &s, 0);
  nx_json_free(o);

  if (write_file(file, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH, s.s, s.size) == -1) {
    return err_stdlib(0, file);
  }

  return err_success();
}

void ServiceConfig_Free(ServiceConfig* c) {
  Mem_Free((char*) c->SelectedConfigId);
  Mem_Free(c->TargetFanSpeeds.data);
  for_each_array(FanTemperatureSourceConfig*, ftsc, c->FanTemperatureSources) {
    for_each_array(const char**, s, ftsc->Sensors)
      Mem_Free((char*) *s);
    Mem_Free(ftsc->Sensors.data);
  }
  Mem_Free(c->FanTemperatureSources.data);

  memset(c, 0, sizeof(*c));
}
