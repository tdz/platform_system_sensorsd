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

/* This file contains various PDU-related code and values.
 *
 * |handle_pdu_by_service| and |handle_pdu_by_opcode| call a function
 * from a look-up table, depending on the PDU's service or opcode. Both
 * follow the convention of return '0' on success and '-1' on errors.
 */

#pragma once

#include <pdu/pdu.h>
#include <stdint.h>

struct pdu;

/* |PROTOCOL_VERSION| contains the current version number of the IPC
 * protocol. Increment this number when you modify the protocol.
 */
enum {
  PROTOCOL_VERSION = 1
};

/* This enumerator lists the available services.
 */
enum {
  SERVICE_REGISTRY = 0x00,
  SERVICE_POLL = 0x01
};

/* The error codes are kept in sync with Bluetooth status to make
 * development a bit easier. If you need additional error code, add
 * them to the end.
 */
enum {
  ERROR_NONE = 0x0,
  ERROR_FAIL = 0x1,
  ERROR_NOT_READY = 0x2,
  ERROR_NOMEM = 0x3,
  ERROR_BUSY = 0x4,
  ERROR_DONE = 0x5,
  ERROR_UNSUPPORTED = 0x6,
  ERROR_PARM_INVALID = 0x7
};

int
handle_pdu_by_service(const struct pdu* cmd,
                      int (* const handler[PDU_MAX_NUM_SERVICES])(
                        const struct pdu*));

int
handle_pdu_by_opcode(const struct pdu* cmd,
                     int (* const handler[PDU_MAX_NUM_OPCODES])(
                       const struct pdu*));
