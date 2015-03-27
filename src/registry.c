/*
 * Copyright (C) 2015  Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* This file implements the Registry service. */

#include "registry.h"

#include <assert.h>
#include <pdu/pdubuf.h>

#include "log.h"
#include "pdu.h"
#include "service.h"

enum {
  /* commands/responses */
  OPCODE_REGISTER_MODULE = 0x01,
  OPCODE_UNREGISTER_MODULE = 0x02,
};

enum {
  IPC_FIELD_SIZE_VERSION = 4 /* protocol version */
};

#define OPCODE_REGISTER_MODULE_RSP_SIZE \
  IPC_FIELD_SIZE_VERSION /* error code */

static void (*g_send_pdu)(struct pdu_wbuf* wbuf);

/* |send_pdu| takes a PDU write buffer and hands it over to the send
 * callback. It's save to call this function even if no send callback
 * has been installed.
 */
static void
send_pdu(struct pdu_wbuf* wbuf)
{
  if (!g_send_pdu) {
    ALOGE("g_send_pdu is NULL");
  }

  g_send_pdu(wbuf);
}

/*
 * Commands/Responses
 *
 * The functions below implement command handling for the Registry module. They
 * receive a command PDU form the protocol framework, execute the command, and
 * reply with the corresponding response PDU.
 *
 * The Registry service supports the commands |register_module|, which allows
 * clients to register another service, and |unregister_module|, which allows
 * to remove a registered service.
 *
 * Both functions follows the usual conventions for the command callbacks.
 * They receive a PDU as the argument and unpack the contained data. In both
 * cases, this is the service id. Both functions answer by sending a reply.
 * In the case of |register_module| the reply includes the version of the
 * IPC protocol. Clients should check this version for compatibility with
 * their own implementation.
 *
 * The functions return ERROR_NONE on success, or an error code on failure. In
 * the later case, the protocol framework will send out the error response to the
 * client.
 */

static int
register_module(const struct pdu* cmd)
{
  uint8_t service;
  struct pdu_wbuf* wbuf;
  int (*handler)(const struct pdu*);

  if (read_pdu_at(cmd, 0, "C", &service) < 0) {
    return ERROR_PARM_INVALID;
  }

  if (g_service_handler[service]) {
    ALOGE("service 0x%x already registered", service);
    return ERROR_FAIL;
  }

  if (!g_register_service[service]) {
    ALOGE("invalid service id 0x%x", service);
    return ERROR_FAIL;
  }

  wbuf = create_pdu_wbuf(OPCODE_REGISTER_MODULE_RSP_SIZE, 0, NULL);

  if (!wbuf) {
    return ERROR_NOMEM;
  }

  handler = g_register_service[service](g_send_pdu);

  if (!handler) {
    goto err_register_service;
  }

  g_service_handler[service] = handler;

  init_pdu(&wbuf->buf.pdu, cmd->service, cmd->opcode);

  if (append_to_pdu(&wbuf->buf.pdu, "I", (uint32_t)PROTOCOL_VERSION) < 0) {
    goto err_append_to_pdu;
  }

  send_pdu(wbuf);

  return ERROR_NONE;

err_append_to_pdu:
err_register_service:
  destroy_pdu_wbuf(wbuf);
  return ERROR_FAIL;
}

static int
unregister_module(const struct pdu* cmd)
{
  uint8_t service;
  struct pdu_wbuf* wbuf;

  if (read_pdu_at(cmd, 0, "C", &service) < 0) {
    return ERROR_PARM_INVALID;
  }

  wbuf = create_pdu_wbuf(0, 0, NULL);

  if (!wbuf) {
    return ERROR_NOMEM;
  }

  if (service == SERVICE_REGISTRY) {
    ALOGE("service REGISTRY cannot be unregistered");
    goto err_service_registry;
  }

  if (!g_unregister_service[service]) {
    ALOGE("service 0x%x not registered", service);
    goto err_not_unregister_service;
  }

  if (g_unregister_service[service]() < 0) {
    goto err_unregister_service;
  }

  g_service_handler[service] = NULL;

  init_pdu(&wbuf->buf.pdu, cmd->service, cmd->opcode);
  send_pdu(wbuf);

  return ERROR_NONE;

err_unregister_service:
err_not_unregister_service:
err_service_registry:
  destroy_pdu_wbuf(wbuf);
  return ERROR_FAIL;
}

/*
 * Registry module framework
 *
 * The functions below implement the general framwwork for the Regsitry
 * module. The functions |init_registry| and |uninit_regsitry| are both
 * documented in the header file.
 *
 * |registry_handler| is the service-handler function of the Regsitry
 * service. It's called by the protocol framework for Registry command
 * PDUs. Its parameters and return value are the same as for the command
 * handlers.
 */

static int
registry_handler(const struct pdu* cmd)
{
  static int (* const handler[PDU_MAX_NUM_OPCODES])(const struct pdu*) = {
    [OPCODE_REGISTER_MODULE] = register_module,
    [OPCODE_UNREGISTER_MODULE] = unregister_module
  };

  return handle_pdu_by_opcode(cmd, handler);
}

int
init_registry(void (*send_pdu_cb)(struct pdu_wbuf*))
{
  assert(send_pdu_cb);

  g_send_pdu = send_pdu_cb;
  g_service_handler[SERVICE_REGISTRY] = registry_handler;

  return 0;
}

void
uninit_registry()
{
  g_service_handler[SERVICE_REGISTRY] = NULL;
  g_send_pdu = NULL;
}
