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

#pragma once

/*
 * This file contains the public interface to the I/O framework. Call
 * |init_io| after starting the I/O loop to connect to the client. The
 * socket name is passed as argument. |init_io| follows the convention
 * of returning 0 for success and -1 for an error. In the latter case,
 * the daemon should exit signalling failure.
 *
 * To clean up the I/O structures during shutdown, call |uninit_io|.
 */

int
init_io(const char* socket_name);

void
uninit_io(void);
