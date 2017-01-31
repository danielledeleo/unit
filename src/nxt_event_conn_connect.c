
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>


void
nxt_event_conn_connect(nxt_task_t *task, nxt_event_conn_t *c)
{
    void                *data;
    nxt_event_engine_t  *engine;

    data = c->socket.data;
    engine = task->thread->engine;

    if (engine->batch != 0) {
        nxt_work_queue_add(&engine->socket_work_queue,
                           nxt_event_conn_batch_socket, task, c, data);
        return;
    }

    if (nxt_event_conn_socket(task, c) == NXT_OK) {
        c->io->connect(task, c, data);
        return;
    }

    c->write_state->error_handler(task, c, data);
}


void
nxt_event_conn_batch_socket(nxt_task_t *task, void *obj, void *data)
{
    nxt_event_conn_t    *c;
    nxt_work_handler_t  handler;

    c = obj;

    if (nxt_event_conn_socket(task, c) == NXT_OK) {
        c->socket.write_work_queue = c->write_work_queue;
        handler = c->io->connect;

    } else {
        handler = c->write_state->error_handler;
    }

    nxt_work_queue_add(&task->thread->engine->connect_work_queue,
                       handler, task, c, data);
}


void
nxt_event_conn_io_connect(nxt_task_t *task, void *obj, void *data)
{
    nxt_event_conn_t              *c;
    nxt_work_handler_t            handler;
    nxt_event_engine_t            *engine;
    const nxt_event_conn_state_t  *state;

    c = obj;

    state = c->write_state;

    switch (nxt_socket_connect(c->socket.fd, c->remote)) {

    case NXT_OK:
        c->socket.write_ready = 1;
        handler = state->ready_handler;
        break;

    case NXT_AGAIN:
        c->socket.write_handler = nxt_event_conn_connect_test;
        c->socket.error_handler = state->error_handler;

        engine = task->thread->engine;

        nxt_event_conn_timer(engine, c, state, &c->write_timer);

        nxt_event_fd_enable_write(engine, &c->socket);
        return;

    case NXT_DECLINED:
        handler = state->close_handler;
        break;

    default: /* NXT_ERROR */
        handler = state->error_handler;
        break;
    }

    nxt_event_conn_io_handle(task->thread, c->write_work_queue, handler, task,
                             c, data);
}


nxt_int_t
nxt_event_conn_socket(nxt_task_t *task, nxt_event_conn_t *c)
{
    nxt_uint_t    family;
    nxt_socket_t  s;

    nxt_debug(task, "event conn socket");

    family = c->remote->u.sockaddr.sa_family;

    s = nxt_socket_create(family, c->remote->type, 0, NXT_NONBLOCK);

    if (nxt_slow_path(s == -1)) {
        return NXT_ERROR;
    }

    c->sendfile = 1;

#if (NXT_HAVE_UNIX_DOMAIN && NXT_SOLARIS)

    if (family == AF_UNIX) {
        /* Solaris AF_UNIX does not support sendfilev(). */
        c->sendfile = 0;
    }

#endif

    c->socket.fd = s;

    c->socket.task = task;
    c->read_timer.task = task;
    c->write_timer.task = task;

    if (c->local != NULL) {
        if (nxt_slow_path(nxt_socket_bind(s, c->local, 0) != NXT_OK)) {
            nxt_socket_close(s);
            return NXT_ERROR;
        }
    }

    return NXT_OK;
}


void
nxt_event_conn_connect_test(nxt_task_t *task, void *obj, void *data)
{
    int               ret, err;
    socklen_t         len;
    nxt_event_conn_t  *c;

    c = obj;

    nxt_debug(task, "event connect test fd:%d", c->socket.fd);

    nxt_event_fd_block_write(task->thread->engine, &c->socket);

    if (c->write_state->autoreset_timer) {
        nxt_timer_disable(task->thread->engine, &c->write_timer);
    }

    err = 0;
    len = sizeof(int);

    /*
     * Linux and BSDs return 0 and store a pending error in the err argument;
     * Solaris returns -1 and sets the errno.
     */

    ret = getsockopt(c->socket.fd, SOL_SOCKET, SO_ERROR, (void *) &err, &len);

    if (nxt_slow_path(ret == -1)) {
        err = nxt_errno;
    }

    if (err == 0) {
        nxt_event_conn_io_handle(task->thread, c->write_work_queue,
                                 c->write_state->ready_handler, task, c, data);
        return;
    }

    c->socket.error = err;

    nxt_log(task, nxt_socket_error_level(err, c->socket.log_error),
            "connect(%d, %*s) failed %E",
            c->socket.fd, c->remote->text_len, c->remote->text, err);

    nxt_event_conn_connect_error(task, c, data);
}


void
nxt_event_conn_connect_error(nxt_task_t *task, void *obj, void *data)
{
    nxt_event_conn_t              *c;
    nxt_work_handler_t            handler;
    const nxt_event_conn_state_t  *state;

    c = obj;

    state = c->write_state;

    switch (c->socket.error) {

    case NXT_ECONNREFUSED:
#if (NXT_LINUX)
    case NXT_EAGAIN:
        /*
         * Linux returns EAGAIN instead of ECONNREFUSED
         * for UNIX sockets if a listen queue is full.
         */
#endif
        handler = state->close_handler;
        break;

    default:
        handler = state->error_handler;
        break;
    }

    nxt_event_conn_io_handle(task->thread, c->write_work_queue, handler,
                             task, c, data);
}
