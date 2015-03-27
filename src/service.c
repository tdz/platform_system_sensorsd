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
 * This file contains data structures for mapping service ids to
 * functions.
 *
 * |g_register_service| and |g_unregister_service| contain each service's
 * callback for initializing and uninitializing the service. The only
 * exception is the Regsitry service itself, which is initialized from
 * within the I/O framework.
 *
 * |g_service_handler| contains each service's callbacks for handling
 * a received PDU. It's filled by the Registry service when the client
 * process registers a service.
 */

#include "service.h"

int (*g_service_handler[PDU_MAX_NUM_SERVICES])(const struct pdu*);

register_func (* const g_register_service[PDU_MAX_NUM_SERVICES])(
  void (*)(struct pdu_wbuf*)) = {
  /* SERVICE_REGISTRY is special and not handled here */
};

int (*g_unregister_service[PDU_MAX_NUM_SERVICES])() = {
};
