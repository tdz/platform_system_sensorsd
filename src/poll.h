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

/*
 * This file contains the interface to the Poll service.
 *
 * Two functions are provided: |register_poll| and |unregister_poll|
 * for registering and unregistering the service in the protocol framework.
 *
 * For registering, |register_poll| takes a callback for sending PDUs
 * and returns Poll's service-handler function on success. On errors,
 * NULL is retured.
 *
 * After the register function returns successfully, Poll will be able to
 * process commands or send out notifications. For commands, the protocol
 * framework calls the service handler when it received a command for the
 * Poll service.
 *
 * The function for unregistering, |unregister_poll|, frees the service's
 * resources. Once it returns successfully with a result of 0, no commands
 * can be processed by the service handler and no notifications will be
 * generated by the service. On error, -1 is returned.
 */

#pragma once

struct pdu;
struct pdu_wbuf;

int
(*register_poll(void (*send_pdu_cb)(struct pdu_wbuf*)))(const struct pdu*);

int
unregister_poll(void);
