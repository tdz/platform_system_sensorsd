/*
 * Copyright (C) 2015-2016  Mozilla Foundation
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

/* This file implements various PDU-related interfaces. See the corresponding
 * header file for documentation.
 */

#include "pdu.h"

#include <assert.h>
#include <stdarg.h>

#include "log.h"

static int
handle_pdu(const char* field, uint8_t value, const struct pdu* cmd,
           int (* const handler[])(const struct pdu*), size_t nhandlers)
{
  assert(field);
  assert(cmd);
  assert(handler);

  if (value >= nhandlers) {
    ALOGE("%s 0x%x exceeds range", field, value);
    return ERROR_UNSUPPORTED;
  }

  if (!handler[value]) {
    ALOGE("unsupported %s 0x%x", field, value);
    return ERROR_UNSUPPORTED;
  }

  return handler[value](cmd);
}

int
handle_pdu_by_service(const struct pdu* cmd,
                      int (* const handler[PDU_MAX_NUM_SERVICES])(
                        const struct pdu*))
{
  return handle_pdu("service", cmd->service, cmd,
                    handler, PDU_MAX_NUM_SERVICES);
}

int
handle_pdu_by_opcode(const struct pdu* cmd,
                     int (* const handler[PDU_MAX_NUM_OPCODES])(
                       const struct pdu*))
{
  return handle_pdu("opcode", cmd->opcode, cmd,
                    handler, PDU_MAX_NUM_OPCODES);
}
