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

/*
 * This file contains the public interface to the Registry service.
 *
 * Two functions are provided: |init_registry| and |uninit_registry| for
 * registering and unregistering the service in the protocol framework.
 *
 * For registering, |init_registry| takes a callback for sending PDUs
 * and returns '0' on success. On errors, '-1' is retured. It sets up
 * the registry's internal state to process further commands. Once the
 * function returns succesfully, the Registry service will be able to
 * process commands. The init funtion's only parameter is a pointer to
 * the PDU send function of the I/O framework. The service will send out
 * its PDUs using this function and forward the address to any service
 * that is registered by the client.
 *
 * The function for unregistering, |unregister_poll|, frees the service's
 * resources. Once it returns no commands can be processed.
 *
 * The public interface of the Registry service is different from other
 * services as it does not return a function pointer when registering.
 * This is because Registry is supposed to be initialized during start
 * up and not by a command from the client. Clients can expect this
 * service to be available all the time.
 */

#pragma once

struct pdu_wbuf;

int
init_registry(void (*send_pdu_cb)(struct pdu_wbuf*));

void
uninit_registry(void);
