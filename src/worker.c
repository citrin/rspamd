/*
 * Copyright (c) 2009, Rambler media
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY Rambler media ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Rambler BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Rspamd worker implementation
 */

#include "config.h"
#include "util.h"
#include "main.h"
#include "protocol.h"
#include "upstream.h"
#include "cfg_file.h"
#include "url.h"
#include "modules.h"
#include "message.h"

#include <EXTERN.h>               /* from the Perl distribution     */
#include <perl.h>                 /* from the Perl distribution     */

#define TASK_POOL_SIZE 4095
/* 2 seconds for worker's IO */
#define WORKER_IO_TIMEOUT 2

const f_str_t CRLF = {
	/* begin */"\r\n",
	/* len */2,
	/* size */2
};

static struct timeval io_tv;

extern PerlInterpreter *perl_interpreter;

static void write_socket (void *arg);

static 
void sig_handler (int signo)
{
	switch (signo) {
		case SIGINT:
		case SIGTERM:
			_exit (1);
			break;
	}
}

/*
 * Config reload is designed by sending sigusr to active workers and pending shutdown of them
 */
static void
sigusr_handler (int fd, short what, void *arg)
{
	struct rspamd_worker *worker = (struct rspamd_worker *)arg;
	/* Do not accept new connections, preparing to end worker's process */
	struct timeval tv;
	tv.tv_sec = SOFT_SHUTDOWN_TIME;
	tv.tv_usec = 0;
	event_del (&worker->sig_ev);
	event_del (&worker->bind_ev);
	do_reopen_log = 1;
	msg_info ("worker's shutdown is pending in %d sec", SOFT_SHUTDOWN_TIME);
	event_loopexit (&tv);
	return;
}

/*
 * Destructor for recipients list
 */
static void
rcpt_destruct (void *pointer)
{
	struct worker_task *task = (struct worker_task *)pointer;

	if (task->rcpt) {
		g_list_free (task->rcpt);
	}
}

/*
 * Free all structures of worker_task
 */
static void
free_task (struct worker_task *task)
{
	GList *part;
	struct mime_part *p;

	if (task) {
		msg_debug ("free_task: free pointer %p", task);
		if (task->memc_ctx) {
			memc_close_ctx (task->memc_ctx);
		}
		while ((part = g_list_first (task->parts))) {
			task->parts = g_list_remove_link (task->parts, part);
			p = (struct mime_part *)part->data;
			g_byte_array_free (p->content, FALSE);
			g_list_free_1 (part);
		}
		memory_pool_delete (task->task_pool);
		rspamd_remove_dispatcher (task->dispatcher);
		close (task->sock);
		g_free (task);
	}
}

/*
 * Callback that is called when there is data to read in buffer
 */
static void
read_socket (f_str_t *in, void *arg)
{
	struct worker_task *task = (struct worker_task *)arg;
	ssize_t r;

	switch (task->state) {
		case READ_COMMAND:
		case READ_HEADER:
			if (read_rspamd_input_line (task, in) != 0) {
				task->last_error = "Read error";
				task->error_code = RSPAMD_NETWORK_ERROR;
				task->state = WRITE_ERROR;
				write_socket (task);
			}
			break;
		case READ_MESSAGE:
			task->msg = in;
			r = process_message (task);
			r = process_filters (task);
			if (r == -1) {
				task->last_error = "Filter processing error";
				task->error_code = RSPAMD_FILTER_ERROR;
				task->state = WRITE_ERROR;
				write_socket (task);
			}
			else if (r == 0) {
				task->state = WAIT_FILTER;
				rspamd_dispatcher_pause (task->dispatcher);
			}
			else {
				process_statfiles (task);
				write_socket (task);
			}
			break;
	}
}

/*
 * Callback for socket writing
 */
static void
write_socket (void *arg)
{
	struct worker_task *task = (struct worker_task *)arg;
	
	switch (task->state) {
		case WRITE_REPLY:
			write_reply (task);
			task->state = CLOSING_CONNECTION;
			break;
		case WRITE_ERROR:
			write_reply (task);
			task->state = CLOSING_CONNECTION;
			break;
		case CLOSING_CONNECTION:
			msg_debug ("write_socket: normally closing connection");
			free_task (task);
			break;
		default:
			msg_info ("write_socket: abnormally closing connection");
			free_task (task);
			break;
	}
}

/*
 * Called if something goes wrong
 */
static void
err_socket (GError *err, void *arg)
{
	struct worker_task *task = (struct worker_task *)arg;
	msg_info ("err_socket: abnormally closing connection, error: %s", err->message);
	/* Free buffers */
	free_task (task);
}

/*
 * Accept new connection and construct task
 */
static void
accept_socket (int fd, short what, void *arg)
{
	struct rspamd_worker *worker = (struct rspamd_worker *)arg;
	struct sockaddr_storage ss;
	struct worker_task *new_task;
	socklen_t addrlen = sizeof(ss);
	int nfd;

	if ((nfd = accept (fd, (struct sockaddr *)&ss, &addrlen)) == -1) {
		return;
	}
	if (event_make_socket_nonblocking(fd) < 0) {
		return;
	}
	
	new_task = g_malloc (sizeof (struct worker_task));
	if (new_task == NULL) {
		msg_err ("accept_socket: cannot allocate memory for task, %m");
		return;
	}
	bzero (new_task, sizeof (struct worker_task));
	new_task->worker = worker;
	new_task->state = READ_COMMAND;
	new_task->sock = nfd;
	new_task->cfg = worker->srv->cfg;
	TAILQ_INIT (&new_task->urls);
	new_task->task_pool = memory_pool_new (memory_pool_get_size ());
	/* Add destructor for recipients list (it would be better to use anonymous function here */
	memory_pool_add_destructor (new_task->task_pool, (pool_destruct_func)rcpt_destruct, new_task);
	new_task->results = g_hash_table_new (g_str_hash, g_str_equal);
	memory_pool_add_destructor (new_task->task_pool, (pool_destruct_func)g_hash_table_destroy, new_task->results);
	worker->srv->stat->connections_count ++;

	/* Set up dispatcher */
	new_task->dispatcher = rspamd_create_dispatcher (nfd, BUFFER_LINE, read_socket,
														write_socket, err_socket, &io_tv,
														(void *)new_task);
}

/*
 * Start worker process
 */
void
start_worker (struct rspamd_worker *worker, int listen_sock)
{
	struct sigaction signals;
	int i;

	worker->srv->pid = getpid ();
	worker->srv->type = TYPE_WORKER;
	event_init ();
	g_mime_init (0);

	init_signals (&signals, sig_handler);
	sigprocmask (SIG_UNBLOCK, &signals.sa_mask, NULL);

	/* SIGUSR2 handler */
	signal_set (&worker->sig_ev, SIGUSR2, sigusr_handler, (void *) worker);
	signal_add (&worker->sig_ev, NULL);

	/* Accept event */
	event_set(&worker->bind_ev, listen_sock, EV_READ | EV_PERSIST, accept_socket, (void *)worker);
	event_add(&worker->bind_ev, NULL);

	/* Perform modules configuring */
	for (i = 0; i < MODULES_NUM; i ++) {
		modules[i].module_config_func (worker->srv->cfg);
	}

	/* Send SIGUSR2 to parent */
	kill (getppid (), SIGUSR2);

	io_tv.tv_sec = WORKER_IO_TIMEOUT;
	io_tv.tv_usec = 0;

	event_loop (0);
}

/* 
 * vi:ts=4 
 */
