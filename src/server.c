#include "server.h"

#include "nbfc.h"
#include "nxjson_utils.h"
#include "reverse_nxjson.h"
#include "error.h"
#include "service.h"
#include "log.h"
#include "quit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <signal.h>

static int                Server_FD = -1;
static struct sockaddr_un Server_Address;
static pthread_t          Server_Thread_ID;
static const int          Server_Max_Failures = 100;

static void* Server_Run(void*);

static Error* Server_Command_Set_Fan(int socket, const nx_json* json) {
  int fan = -1;
  float speed = -2;
  const int fancount = Service_Model_Config.FanConfigurations.size;

  nx_json_for_each(c, json) {
    if (!strcmp(c->key, "Command"))
      continue;
    else if (!strcmp(c->key, "Fan")) {
      if (c->type != NX_JSON_INTEGER)
        return err_string(0, "Fan: Not an integer");

      fan = c->val.i;

      if (fan < 0)
        return err_string(0, "Fan: Cannot be negative");
      else if (fan >= fancount)
        return err_string(0, "Fan: No such fan available");
    }
    else if (!strcmp(c->key, "Speed")) {
      if (c->type == NX_JSON_STRING && !strcmp(c->val.text, "auto")) {
        speed = -1;
        continue;
      }
      else if (c->type == NX_JSON_DOUBLE)
        speed = c->val.dbl;
      else if (c->type == NX_JSON_INTEGER)
        speed = c->val.i;
      else {
        return err_string(0, "Speed: Invalid type. Either float or 'auto'");
      }

      if (speed < 0.0 || speed > 100.0)
        return err_string(0, "Speed: Invalid value");
    }
    else {
      return err_string(0, "Unknown arguments");
    }
  }

  if (speed == -2)
    return err_string(0, "Missing argument: Speed");

  for_enumerate_array(int, i, Service_Fans) {
    if (fan == -1 || fan == i) {
      if (speed == -1)
        Fan_SetAutoSpeed(&Service_Fans.data[i].Fan);
      else
        Fan_SetFixedSpeed(&Service_Fans.data[i].Fan, speed);

      if (! options.read_only)
        Fan_ECFlush(&Service_Fans.data[i].Fan);
    }
  }

  Error* e = Service_WriteTargetFanSpeedsToConfig();
  if (e)
    return e;

  nx_json root = {0};
  nx_json *o = create_json_object(NULL, &root);
  create_json_string("Status", o, "OK");

  e = Protocol_Send_Json(socket, o);
  nx_json_free(o);
  return e;
}

static Error* Server_Command_Status(int socket, const nx_json* json) {
  if (json->val.children.length > 1)
      return err_string(0, "Unknown arguments");

  nx_json root = {0};
  nx_json *o = create_json_object(NULL, &root);
  create_json_integer("PID", o, getpid());
  create_json_string("SelectedConfigId", o, service_config.SelectedConfigId);
  create_json_bool("ReadOnly", o, options.read_only);
  nx_json* fans = create_json_array("Fans", o);

  for_each_array(FanTemperatureControl*, ftc, Service_Fans) {
    const Fan* fan = &ftc->Fan;
    nx_json* fan_json = create_json_object(NULL, fans);
    create_json_string("Name", fan_json, fan->fanConfig->FanDisplayName);
    create_json_double("Temperature", fan_json, ftc->Temperature);
    create_json_bool("AutoMode", fan_json, (fan->mode == Fan_ModeAuto));
    create_json_bool("Critical", fan_json, fan->isCritical);
    create_json_double("CurrentSpeed", fan_json, Fan_GetCurrentSpeed(fan));
    create_json_double("TargetSpeed", fan_json, Fan_GetTargetSpeed(fan));
    create_json_double("RequestedSpeed", fan_json, Fan_GetRequestedSpeed(fan));
    create_json_integer("SpeedSteps", fan_json, Fan_GetSpeedSteps(fan));
  }

  Error* e = Protocol_Send_Json(socket, o);
  nx_json_free(o);
  return e;
}

static void* Server_Handle_Client(void* arg) {
  const int socket = (int) (intptr_t) arg;
  const nx_json* json = NULL;
  char* buf = NULL;
  Error* e;

  e = Protocol_Receive_Json(socket, &buf, &json);
  if (e)
    goto error;

  if (json->type != NX_JSON_OBJECT) {
    e = err_string(0, "Not a JSON object");
    goto error;
  }

  const nx_json* command = nx_json_get(json, "Command");
  if (! command) {
    e = err_string(0, "Missing 'Command' field");
    goto error;
  }

  if (command->type != NX_JSON_STRING) {
    e = err_string(0, "Command: not a string");
    goto error;
  }

  pthread_mutex_lock(&Service_Lock);
  {
    if (!strcmp(command->val.text, "set-fan-speed"))
      e = Server_Command_Set_Fan(socket, json);
    else if (!strcmp(command->val.text, "status"))
      e = Server_Command_Status(socket, json);
    else
      e = err_string(0, "Invalid command");
  }
  pthread_mutex_unlock(&Service_Lock);

error:
  if (e)
    Protocol_Send_Error(socket, err_print_all(e));

  Mem_Free(buf);
  nx_json_free(json);
  close(socket);

  return NULL;
}

Error* Server_Init() {
  Error* e = NULL;

  memset(&Server_Address, 0, sizeof(Server_Address));
  Server_Address.sun_family = AF_UNIX;
  snprintf(Server_Address.sun_path, sizeof(Server_Address.sun_path), NBFC_SOCKET_PATH);

  if ((Server_FD = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
    e = err_stdlib(0, "socket()");
    goto error;
  }

  if (bind(Server_FD, (struct sockaddr *)&Server_Address, sizeof(Server_Address)) < 0) {
    e = err_stdlib(err_string(0, NBFC_SOCKET_PATH), "bind()");
    goto error;
  }

  if (chmod(NBFC_SOCKET_PATH, 0666) < 0) {
    e = err_stdlib(err_string(0, NBFC_SOCKET_PATH), "chmod()");
    goto error;
  }

  if (listen(Server_FD, 3) < 0) {
    e = err_stdlib(0, "listen()");
    goto error;
  }

error:
  if (e)
    Server_Close();

  return e;
}

Error* Server_Start() {
  if (pthread_create(&Server_Thread_ID, NULL, Server_Run, NULL) != 0) {
    return err_stdlib(0, "pthread_create()");
  }

  pthread_detach(Server_Thread_ID);
  return err_success();
}

void Server_Stop() {
  pthread_kill(Server_Thread_ID, SIGTERM);
}

Error* Server_Loop() {
  int addrlen = sizeof(Server_Address);
  int new_socket;
  pthread_t thread_id;

  if ((new_socket = accept(Server_FD, (struct sockaddr*)&Server_Address, (socklen_t*)&addrlen)) < 0)
    return err_stdlib(0, "accept()");

  if (pthread_create(&thread_id, NULL, Server_Handle_Client, (void*) (intptr_t) new_socket) != 0) {
    close(new_socket);
    return err_stdlib(0, "pthread_create()");
  }

  pthread_detach(thread_id);

  return err_success();
}

static void* Server_Run(void*) {
  int failures = 0;

  while (! quit) {
    Error* e = Server_Loop();

    // When the server stops, accept() from Server_Loop() returns
    // EBADF. So don't handle this as an error.
    if (! quit)
      e_warn();

    if (e) {
      if (++failures > Server_Max_Failures) {
        quit = 1;
        return NULL;
      }
    }
    else
      failures = 0;
  }

  return NULL;
}

void Server_Close() {
  if (Server_FD != -1) {
    close(Server_FD);
    unlink(NBFC_SOCKET_PATH);
    Server_FD = -1;
  }
}
