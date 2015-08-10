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

/* This file contains helper macros for pointers and arrays. */

#pragma once

#include <stddef.h>

/* Returns the number of elements in an array.
 */
#define ARRAY_LENGTH(_array) \
  ( sizeof(_array) / sizeof((_array)[0]) )

/* Returns the container around a field of the container's structure.
 *
 *  _t: the container's type
 *  _v: the address of the field
 *  _m: name of the field in the container (i.e, the container's member)
 */
#define CONTAINER(_t, _v, _m) \
  ( (_t*)( ((const unsigned char*)(_v)) - offsetof(_t, _m) ) )
