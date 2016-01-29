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

/* This file contains the main entry point and setup code. */

#include <assert.h>
#include <fdio/loop.h>
#include <fdio/task.h>
#include <hardware_legacy/power.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "compiler.h"
#include "io.h"
#include "log.h"
#include "memptr.h"
#include "wakelock.h"

/*
 * Command-line options
 *
 * The functions below parse the command-line options into an options
 * structure. The entry point for this functionality is |parse_opts|,
 * which takes the options count and array as input, and an options
 * structure for returning the parsed values.
 *
 * For each supported option, there's a |parse_opt_*| function. None
 * of these function should be called from outside of |parse_opts|. We
 * currently only support 'a' for settings the daemons network address
 * and 'h' for printing general information about the program.
 *
 * The return value of the parser functions differ slightly from the
 * usual conventions. A value of '0' means success and a value of '-1'
 * means 'abort with error'; like in other functions. But there's also
 * '1', which mean 'exit with success'. Returning '1' from the parser
 * function will terminate the program reporting success. That's helpful
 * for options that don't want sensorsd to enter its main loop, such as
 * 'h'.
 */

struct options {
  const char* socket_name;
};

static int
parse_opt_question_mark(int c)
{
  fprintf(stderr, "Unknown option %c\n", c);

  return -1;
}

static int
parse_opt_a(char* arg, struct options* opt)
{
  if (!arg) {
    fprintf(stderr, "Error: No network address specified.");
    return -1;
  }

  if (!strlen(arg)) {
    fprintf(stderr, "Error: The specified network address is empty.");
    return -1;
  }

  opt->socket_name = arg;

  return 0;
}

static int
parse_opt_h(void)
{
  printf("Usage: sensorsd [OPTION]\n"
         "Wraps Android sensors drivers behind a networking protocol\n"
         "\n"
         "General options:\n"
         "  -h    displays this help\n"
         "\n"
         "Networking:\n"
         "  -a    the network address\n"
         "\n"
         "The only supported address family is AF_UNIX with abstract "
         "names.\n");

  return 1;
}

static int
parse_opt(int c, char* arg, struct options* options)
{
  switch (c) {
    case '?':
      return parse_opt_question_mark(c);
    case 'a':
      return parse_opt_a(arg, options);
    case 'h':
      return parse_opt_h();
  }
  return -1;
}

static int
parse_opts(int argc, char* argv[], struct options* options)
{
  int saved_opterr, res;

  assert(options);

  saved_opterr = opterr;
  opterr = 0; /* no default error messages from getopt */

  res = 0;

  do {
    int c = getopt(argc, argv, "a:h");
    if (c < 0) {
      break; /* end of options */
    }
    res = parse_opt(c, optarg, options);
  } while (!res);

  opterr = saved_opterr;

  return res;
}

/*
 * Initialization and program start up
 *
 * After starting the program, the Android kernel gives us a few
 * milliseconds until it suspends the system. So the first thing
 * Sensorsd's |main| function does is to acquire a wake lock to
 * ensure progress during the initialization.
 *
 * Next it starts parsing the command-line arguments by calling
 * |parse_opts|. If command-line arguments are OK, sensorsd is ready
 * to turn it self into a system service.
 *
 * The first step towards becoming a system service is a call to
 * |make_daemon|, which uncouples sensorsd from its environment as
 * far as that's possible. The function contains comments for each
 * performed step. We cannot use the Unix function |daemon|, which
 * offers similar functionality. |daemon| forks the process, and the
 * parent process exists. The Android init system would interpret
 * this as sensorsd having stopped, while it's actually running in
 * the child process.
 *
 * Next, sensorsd starts its main loop and opens a socket connection
 * to the client program. |epoll_loop| is part of libfdio and handles
 * all I/O and events. We never leave it during normal operation.
 *
 * Initialization is performed by |init|. If first sets up the task
 * queue, which also comes with libfdio, and then opens the socket
 * to the client with a call to |init_io|. We need to do all these
 * operations in the callback, because they require the I/O loop to
 * be running. libfdio contains mor information about |epoll_loop|
 * and how to use it.
 *
 * After |init_io| returned successfully, we should have a pending
 * connection to the client program. It's now safe to drop the wake
 * lock. Any further operation will be triggered by the client and
 * sensorsd will wake up when commands or data are available.
 *
 * When we leave the main I/O loop during program termination, the
 * callback to |uninit| will clean up the resources. It won't run
 * if |init| failed, so anything we set up in |init| can be expected
 * to be initialized.
 *
 * Both |init| and |uninit| receive a pointer to arbitrary data. In
 * the case of sensorsd, its the options structure with the program's
 * configuration.
 *
 * Finally some words about multithreading:
 *
 * All functions are expected to run on the thread that contains the
 * main I/O loop and none of the data structures are thread-safe by
 * default. All IPC and I/O should be executed within the I/O loop
 * *without* blocking the loop! See the header files of libfdio for
 * the related documentation.
 *
 * Exceptions to the I/O-thread-only rule are allowed for individual
 * services. Some services might need to block when polling device
 * drivers, or as part of the drivers interface. Blocking operation
 * should be performed on a separate thread. Don't call the I/O loop
 * from this thread. Instead call |run_task| with the function and
 * data to execute. |run_task| sends the task to the I/O thread where
 * it is executed within the I/O loop.
 */

static void
make_daemon(void)
{
  /* Create new session; disconnect from controlling terminal */
  if ((setsid() < 0 && errno != EPERM)) {
    ALOGW_ERRNO("setsid");
  }

  /* Clear file creation mask */
  umask(0);

  /* Change to root dir; allow unmounting previous working directory */
  if (chdir("/") < 0) {
    ALOGW_ERRNO("chdir");
  }

  /* Normally we would now close all open file descriptors and re-
   * open the standard file descriptors to '/dev/null'. On Android,
   * this breaks the logging system, so we leave the file descriptors
   * open.
   */
}

static enum ioresult
init(void* data)
{
  const struct options* options = data;

  if (init_task_queue() < 0) {
    return -1;
  }

  if (init_io(options->socket_name) < 0) {
    goto err_init_io;
  }

  /* We should have a pending connection request at this point; enough
   * to wake up the daemon on input. Suspending is OK from now on.
   */
  release_wake_lock(WAKE_LOCK_NAME);

  return IO_OK;

err_init_io:
  uninit_task_queue();
  return IO_ABORT;
}

static void
uninit(void* data ATTRIBS(UNUSED))
{
  uninit_io();
  uninit_task_queue();
}

int
main(int argc, char* argv[])
{
  static const char DEFAULT_SOCKET_NAME[] = "sensorsd";

  int res;
  struct options options = {
    .socket_name = DEFAULT_SOCKET_NAME
  };

  /* Guarantee progress until we opened a connection, or exit. */
  acquire_wake_lock(PARTIAL_WAKE_LOCK, WAKE_LOCK_NAME);

  res = parse_opts(argc, argv, &options);

  if (res) {
    /* going to exit */
    release_wake_lock(WAKE_LOCK_NAME);

    if (res > 0) {
      return EXIT_SUCCESS;
    } else if (res < 0) {
      return EXIT_FAILURE;
    }
  }

  make_daemon();

  if (epoll_loop(init, uninit, &options) < 0) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
