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

/* This file contains the I/O framework for sensorsd. */

#include "io.h"

#include <assert.h>
#include <cutils/sockets.h>
#include <fcntl.h>
#include <fdio/loop.h>
#include <hardware_legacy/power.h>
#include <pdu/pdubuf.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "compiler.h"
#include "log.h"
#include "memptr.h"
#include "pdu.h"
#include "registry.h"
#include "service.h"
#include "wakelock.h"

enum {
  OPCODE_ERROR = 0
};

enum {
  IPC_FIELD_SIZE_ERROR = 1
};

#define OPCODE_ERROR_RSP_SIZE \
  IPC_FIELD_SIZE_ERROR /* error code */

/*
 * I/O state
 *
 * The data structure |struct io_state| contains the state of an active
 * connection: a file descriptor, the epoll callbacks, a receive buffer
 * and a send queue.
 *
 * There's a function for each I/O operation. Call them from your epoll
 * handlers to update the I/O state for the file descriptor. The return
 * values of these functions, if any, is 0 on success and -1 on errors.
 *
 * The function |io_state_err| handles errors and |io_state_hup| handles
 * HUP events. There's not much we can do, except clearing the I/O state
 * and removing the file descriptor from the I/O loop.
 *
 * The functions |io_state_in| and |io_state_out| perform the action I/O
 * operations. Call |io_state_in| when the file descriptor becomes read-
 * able, and |io_state_out| when the file descriptor becomes writeable.
 *
 * |io_state_in| receives an additional parameter with the handler for
 * the received PDU buffer. So when a PDU has been received on the file
 * descriptor, |io_state_in| calls the handler to process the received
 * data.
 *
 * |io_state_out| writes PDU messages to the file descriptor. PDUs are
 * stored in the I/O state's send queue. You cannot directly send PDUs
 * Instead call |io_state_send| with the PDU write buffer to append the
 * PDU to the send queue. |io_state_send| will add the file descriptor
 * to the epoll loop; waiting for writeability. Directly sending a PDU
 * could block the epoll loop, which is to be avoided.
 *
 * Instances of |struct io_state| should always be initialized with a
 * call to |IO_STATE_INITIALIZER| or |INIT_IO_STATE|. The former sets
 *  the file-descriptor field |fd| to '-1', which means 'invalid'.
 */

STAILQ_HEAD(pdu_wbuf_stailq, pdu_wbuf);

struct io_state {
  int fd;
  uint32_t epoll_events;
  struct fd_events* epoll_funcs;
  struct pdu_rbuf* rbuf;
  struct pdu_wbuf_stailq sendq;
};

#define IO_STATE_INITIALIZER(_io_state) \
  { \
    .fd = -1, \
    .epoll_events = 0, \
    .epoll_funcs = NULL, \
    .rbuf = NULL, \
    STAILQ_HEAD_INITIALIZER((_io_state).sendq) \
  }

#define INIT_IO_STATE(_io_state, _fd, _epoll_events, _epoll_funcs, _rbuf) \
  do { \
    struct io_state* __io_state = (_io_state); \
    __io_state->fd = (_fd); \
    __io_state->epoll_events = (_epoll_events); \
    __io_state->epoll_funcs = (_epoll_funcs); \
    __io_state->rbuf = (_rbuf); \
    STAILQ_INIT(&__io_state->sendq); \
  } while (0)

static void
io_state_err(struct io_state* io_state)
{
  assert(io_state);

  if (io_state->rbuf) {
    destroy_pdu_rbuf(io_state->rbuf);
    io_state->rbuf = NULL;
  }

  remove_fd_from_epoll_loop(io_state->fd);
  TEMP_FAILURE_RETRY(close(io_state->fd)); /* no error checks here */
  io_state->epoll_events = 0;
  io_state->fd = -1;
}

static void
io_state_hup(struct io_state* io_state)
{
  assert(io_state);

  if (io_state->rbuf) {
    destroy_pdu_rbuf(io_state->rbuf);
    io_state->rbuf = NULL;
  }

  remove_fd_from_epoll_loop(io_state->fd);

  if (TEMP_FAILURE_RETRY(close(io_state->fd)) < 0) {
    ALOGW_ERRNO("close");
  }

  io_state->epoll_events = 0;
  io_state->fd = -1;
}

static int
io_state_in(struct io_state* io_state,
            int (*handle_pdu)(const struct pdu*, struct io_state*))
{
  struct iovec iv;
  struct msghdr msg;
  ssize_t res;

  assert(io_state);
  assert(handle_pdu);

  acquire_wake_lock(PARTIAL_WAKE_LOCK, WAKE_LOCK_NAME);

  memset(&iv, 0, sizeof(iv));
  iv.iov_base = io_state->rbuf->buf.raw;
  iv.iov_len = io_state->rbuf->maxlen;

  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iv;
  msg.msg_iovlen = 1;

  res = TEMP_FAILURE_RETRY(recvmsg(io_state->fd, &msg, 0));
  if (res < 0) {
    ALOGE_ERRNO("recvmsg");
    goto err_recvmsg;
  }

  if (!res) {
    /* stop watching if peer hung up */

    io_state->epoll_events &= ~EPOLLIN;

    if (io_state->epoll_events) {
      res = add_fd_events_to_epoll_loop(io_state->fd,
                                        io_state->epoll_events,
                                        io_state->epoll_funcs);
      if (res < 0) {
        goto err_add_fd_events_to_epoll_loop;
      }
    } else {
      remove_fd_from_epoll_loop(io_state->fd);
    }
  }

  io_state->rbuf->len = res;

  if (pdu_rbuf_has_pdu(io_state->rbuf)) {
    if (handle_pdu(&io_state->rbuf->buf.pdu, io_state) < 0) {
      goto err_pdu;
    }
    io_state->rbuf->len = 0;
  } else if (pdu_rbuf_is_full(io_state->rbuf)) {
    ALOGE("buffer too small for PDU(0x%x:0x%x)",
          io_state->rbuf->buf.pdu.service, io_state->rbuf->buf.pdu.opcode);
    goto err_pdu;
  }

  release_wake_lock(WAKE_LOCK_NAME);

  return 0;

err_pdu:
err_add_fd_events_to_epoll_loop:
err_recvmsg:
  release_wake_lock(WAKE_LOCK_NAME);
  return -1;
}

static void
io_state_send_pending_wbufs(struct io_state* io_state)
{
  assert(io_state);

  while (!STAILQ_EMPTY(&io_state->sendq)) {
    /* send next pending PDU */

    struct pdu_wbuf* wbuf = STAILQ_FIRST(&io_state->sendq);

    if (!send_pdu_wbuf(wbuf, io_state->fd, 0)) {
      return; /* the operation would block; wait for EPOLLOUT */
    }

    STAILQ_REMOVE_HEAD(&io_state->sendq, stailq);
    destroy_pdu_wbuf(wbuf);
  }
}

static int
io_state_out(struct io_state* io_state)
{
  assert(io_state);

  io_state_send_pending_wbufs(io_state);

  if (STAILQ_EMPTY(&io_state->sendq)) {
    /* stop watching */

    io_state->epoll_events &= ~EPOLLOUT;

    if (io_state->epoll_events) {
      int res = add_fd_events_to_epoll_loop(io_state->fd,
                                            io_state->epoll_events,
                                            io_state->epoll_funcs);
      if (res < 0) {
        return -1; /* there's no good way of handling this failure */
      }
    } else {
      remove_fd_from_epoll_loop(io_state->fd);
    }
  }

  return 0;
}

static int
io_state_send(struct io_state* io_state, struct pdu_wbuf* wbuf)
{
  assert(io_state);

  /* append wbuf to send queue and flush the queue */

  STAILQ_INSERT_TAIL(&io_state->sendq, wbuf, stailq);
  io_state_send_pending_wbufs(io_state);

  if (!STAILQ_EMPTY(&io_state->sendq) &&
      !(io_state->epoll_events & EPOLLOUT)) {

    /* some wbufs remaining; poll file descriptor for writeability */

    uint32_t epoll_events;
    int res;

    epoll_events = io_state->epoll_events | EPOLLOUT;

    res = add_fd_events_to_epoll_loop(io_state->fd,
                                      epoll_events,
                                      io_state->epoll_funcs);
    if (res < 0) {
      return -1; /* there's no good way of handling this failure */
    }

    io_state->epoll_events = epoll_events;
  }

  return 0;
}

/*
 * PDU I/O handling
 */

static void
send_error_reply(uint8_t service, uint8_t error, struct io_state* io_state)
{
  struct pdu_wbuf* wbuf;
  int res;

  wbuf = create_pdu_wbuf(OPCODE_ERROR_RSP_SIZE, 0, NULL);

  if (!wbuf) {
    ALOGE("Could not allocate error PDU; aborting immediately");
    abort();
  }

  init_pdu(&wbuf->buf.pdu, service, OPCODE_ERROR);
  append_to_pdu(&wbuf->buf.pdu, "C", error);

  res = io_state_send(io_state, wbuf);

  if (res < 0) {
    ALOGE("Could not send error PDU; aborting immediately");
    abort();
  }
}

static int
handle_pdu(const struct pdu* cmd, struct io_state* io_state)
{
  int status;

  assert(cmd);

  status = handle_pdu_by_service(cmd, g_service_handler);

  if (status) {
    goto err_handle_pdu_by_service;
  }

  return 0;

err_handle_pdu_by_service:
  send_error_reply(cmd->service, status, io_state);
  return 0; /* signal success because we replied with an error */
}

/*
 * Socket I/O callbacks
 *
 * The functions below implement the callbacks for the connected socket
 * file descriptor. See the header files of libpdu for documentation of
 * the interfaces.
 *
 * We generally don't try to repair I/O errors. If the I/O to the client
 * is broken, we exit the daemon process and let the client recover from
 * the failure.
 */

static enum ioresult
fd_io_err(int fd ATTRIBS(UNUSED), void* data)
{
  struct io_state* io_state;

  assert(data);

  io_state = data;
  assert(io_state->fd == fd);

  io_state_err(io_state);

  return IO_POLL;
}

static enum ioresult
fd_io_hup(int fd ATTRIBS(UNUSED), void* data)
{
  struct io_state* io_state;

  assert(data);

  io_state = data;
  assert(io_state->fd == fd);

  io_state_hup(io_state);

  return IO_EXIT; /* exit with success */
}

static enum ioresult
fd_io_in(int fd ATTRIBS(UNUSED), void* data)
{
  struct io_state* io_state;

  assert(data);

  io_state = data;
  assert(io_state->fd == fd);

  if (io_state_in(io_state, handle_pdu) < 0) {
    return IO_ABORT;
  }

  return IO_OK;
}

static enum ioresult
fd_io_out(int fd ATTRIBS(UNUSED), void* data)
{
  struct io_state* io_state;

  assert(data);

  io_state = data;
  assert(io_state->fd == fd);

  if (io_state_out(io_state) < 0) {
    return IO_ABORT;
  }

  return IO_OK;
}

/*
 * Initialization and connection setup
 *
 * The remainder of the file contains the initialization code for the
 * I/O framework. The public interfaces |init_io| and |uninit_io| are
 * documented in the header file.
 *
 * |uninit_io| will first setup the epoll events for connecting to the
 * client process, and then call |connect_socket|. As the name implies,
 * this function opens the socket connection to the client. With the
 * connection request pending, the function inserts the file descriptor
 * into the epoll main loop. When the request has been accepted by the
 * client, the main loop calls |fd_con_out|. On errors |fd_con_err| is
 * called.
 *
 * |fd_con_out| sets up the file descriptor's I/O state and enters the
 * daemon's main operation. This is inplemented in |connected_socket|
 * and |start_main_loop|. Entering the main operation also registers
 * the Registry service. From this point on, the client is now able to
 * register and unregister further services. The function |send_pdu|
 * is the callback for sending PDU write buffers from a service. The
 * Registry service forwards the pointer to any registered service.
 *
 * All of the functions below return '-1' on errors, except |fd_con_out|
 * and |fd_con_err|, which return an I/O result code. |create_sockaddr_un|
 * returns the size of the created socket address, and |connect_socket|
 * returns the file descriptor on success. The othe functions return '0'
 * on success, if anything.
 *
 * Like during main operation, we don't repair failed connections. If we
 * cannot connect, we quit the daemon and let the client handle the error.
 */

static struct io_state g_io_state[1] = {
  IO_STATE_INITIALIZER(g_io_state[0])
};

static void
send_pdu(struct pdu_wbuf* wbuf)
{
  io_state_send(g_io_state + 0, wbuf);
}

ssize_t
create_sockaddr_un(const char* socket_name, struct sockaddr_un* addr)
{
  static const size_t NAME_OFFSET = 1;

  size_t len, siz, pathsiz;

  assert(socket_name);
  assert(addr);

  /* We get the socket name from the command line, so we perform
   * additional tests against buffer overflows.
   */

  len = strlen(socket_name);

  if (len >= SIZE_MAX) {
    ALOGE("Socket name too long");
    return -1;
  }

  siz = len + 1; /* include trailing '\0' */

  if ((sizeof(addr->sun_path) < siz) ||
      ((sizeof(addr->sun_path) - siz) < NAME_OFFSET)) {
    ALOGE("Socket name too long");
    return -1;
  }

  pathsiz = NAME_OFFSET + siz; /* include leading '\0' of abstract socket */

  if ((pathsiz > (size_t)SSIZE_MAX) ||
      ((SSIZE_MAX - pathsiz) < offsetof(struct sockaddr_un, sun_path))) {
    ALOGE("Socket address too large");
    return -1;
  }

  addr->sun_family = AF_UNIX;
  memset(addr->sun_path, '\0', NAME_OFFSET); /* abstract socket */
  memcpy(addr->sun_path + NAME_OFFSET, socket_name, siz);

  return offsetof(struct sockaddr_un, sun_path) + NAME_OFFSET + siz;
}

static int
connect_socket(const char* socket_name, const struct fd_events* epoll_funcs)
{
  int fd, res, flags;
  ssize_t socklen;
  struct sockaddr_un addr;

  socklen = create_sockaddr_un(socket_name, &addr);

  if (socklen < 0) {
    return -1;
  }

  fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);

  if (fd < 0) {
    ALOGE_ERRNO("socket");
    return -1;
  }

  res = TEMP_FAILURE_RETRY(fcntl(fd, F_GETFD));

  if (res < 0) {
    ALOGE_ERRNO("fcntl(F_GETFD)");
    goto err_fcntl_F_GETFD;
  }

  flags = res | O_CLOEXEC;

  if (TEMP_FAILURE_RETRY(fcntl(fd, F_SETFD, flags)) < 0) {
    ALOGE_ERRNO("fcntl(F_SETFD)");
    goto err_fcntl_F_SETFD;
  }

  res = TEMP_FAILURE_RETRY(fcntl(fd, F_GETFL));

  if (res < 0) {
    ALOGE_ERRNO("fcntl(F_GETFL)");
    goto err_fcntl_F_GETFL;
  }

  flags = res | O_NONBLOCK;

  if (TEMP_FAILURE_RETRY(fcntl(fd, F_SETFL, flags)) < 0) {
    ALOGE_ERRNO("fcntl(F_SETFL)");
    goto err_fcntl_F_SETFL;
  }

  res = TEMP_FAILURE_RETRY(connect(fd,
                                   (const struct sockaddr*)&addr,
                                   socklen));
  if (res < 0) {
    ALOGE_ERRNO("connect");
    goto err_connect;
  }

  if (add_fd_events_to_epoll_loop(fd, EPOLLOUT | EPOLLERR, epoll_funcs) < 0) {
    goto err_add_fd_events_to_epoll_loop;
  }

  return fd;

err_add_fd_events_to_epoll_loop:
err_connect:
err_fcntl_F_SETFL:
err_fcntl_F_GETFL:
err_fcntl_F_SETFD:
err_fcntl_F_GETFD:
  if (TEMP_FAILURE_RETRY(close(fd)) < 0) {
    ALOGW_ERRNO("close");
  }
  return -1;
}

static int
connected_socket(int fd, struct io_state* io_state)
{
  struct pdu_rbuf* rbuf;

  assert(io_state);

  /* Remove fd from current loop to clear callback function */
  remove_fd_from_epoll_loop(fd);

  /* Setup I/O state */

  if (io_state->rbuf) {
    ALOGE("Socket %d is already connected", fd);
    return -1;
  }

  /* We allocate the maximum size for the PDU read buffer (i.e, 64 KiB). We
   * currently don't require that much memory, but we're on the safe side.
   */
  rbuf = create_pdu_rbuf(PDU_MAX_DATA_LENGTH);

  if (!rbuf) {
    return -1;
  }

  io_state->epoll_funcs->epollin = fd_io_in;
  io_state->epoll_funcs->epollout = fd_io_out;
  io_state->epoll_funcs->epollerr = fd_io_err;
  io_state->epoll_funcs->epollhup = fd_io_hup;

  INIT_IO_STATE(io_state, fd, EPOLLERR | EPOLLIN, io_state->epoll_funcs, rbuf);

  return 0;
}

static int
start_main_loop(struct io_state* io_state)
{
  int res;

  assert(io_state);

  if (init_registry(send_pdu) < 0) {
    return -1;
  }

  res = add_fd_events_to_epoll_loop(io_state->fd,
                                    io_state->epoll_events,
                                    io_state->epoll_funcs);
  if (res < 0) {
    goto err_add_fd_events_to_epoll_loop;
  }

  return 0;

err_add_fd_events_to_epoll_loop:
  uninit_registry();
  return -1;
}

static enum ioresult
fd_con_err(int fd ATTRIBS(UNUSED), void* data ATTRIBS(UNUSED))
{
  /* We don't attempt to repair a failed connection request. If an
   * error occures, we abort the daemon and let the client start a
   * new instance.
   */
  return IO_ABORT;
}

static enum ioresult
fd_con_out(int fd, void* data)
{
  struct io_state* io_state;

  assert(data);

  io_state = data;

  if (connected_socket(fd, io_state) < 0) {
    return IO_ABORT;
  }

  if (start_main_loop(io_state) < 0) {
    return IO_ABORT;
  }

  return IO_OK;
}

int
init_io(const char* socket_name)
{
  struct fd_events* epoll_funcs;
  int fd;

  /* free'd in |uninit_io| */
  epoll_funcs = calloc(1, sizeof(*epoll_funcs));

  if (!epoll_funcs) {
    ALOGE_ERRNO("malloc");
    return -1;
  }

  epoll_funcs->epollout = fd_con_out;
  epoll_funcs->epollerr = fd_con_err;
  epoll_funcs->data = g_io_state + 0;

  INIT_IO_STATE(g_io_state + 0, -1, 0, epoll_funcs, NULL);

  fd = connect_socket(socket_name, epoll_funcs);

  if (fd < 0) {
    goto err_connect_socket;
  }

  return 0;

err_connect_socket:
  free(epoll_funcs);
  return -1;
}

void
uninit_io()
{
  size_t i;

  for (i = 0; i < ARRAY_LENGTH(g_io_state); ++i) {
    int res;

    if (g_io_state[i].fd == -1) {
      continue;
    }

    remove_fd_from_epoll_loop(g_io_state[i].fd);

    res = TEMP_FAILURE_RETRY(close(g_io_state[i].fd));
    if (res < 0) {
      ALOGW_ERRNO("close");
    }

    if (g_io_state[i].rbuf) {
      destroy_pdu_rbuf(g_io_state[i].rbuf);
    }

    if (g_io_state[i].epoll_funcs) {
      free(g_io_state[i].epoll_funcs);
    }
  }

  uninit_registry();
}
