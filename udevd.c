/*
 * udevd.c - hotplug event serializer
 *
 * Copyright (C) 2004 Kay Sievers <kay.sievers@vrfy.org>
 * Copyright (C) 2004 Chris Friesen <chris_friesen@sympatico.ca>
 *
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation version 2 of the License.
 *
 *	This program is distributed in the hope that it will be useful, but
 *	WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *	General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License along
 *	with this program; if not, write to the Free Software Foundation, Inc.,
 *	675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <stddef.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <fcntl.h>

#include "list.h"
#include "udev.h"
#include "udev_lib.h"
#include "udev_version.h"
#include "udevd.h"
#include "logging.h"

static int pipefds[2];
static int expected_seqnum = 0;
volatile static int children_waiting;
volatile static int run_msg_q;
volatile static int sig_flag;
static int run_exec_q;

static LIST_HEAD(msg_list);
static LIST_HEAD(exec_list);
static LIST_HEAD(running_list);

static void exec_queue_manager(void);
static void msg_queue_manager(void);
static void user_sighandler(void);
static void reap_kids(void);

#ifdef LOG
unsigned char logname[LOGNAME_SIZE];
void log_message (int level, const char *format, ...)
{
	va_list	args;

	va_start(args, format);
	vsyslog(level, format, args);
	va_end(args);
}
#endif

static void msg_dump_queue(void)
{
#ifdef DEBUG
	struct hotplug_msg *msg;

	list_for_each_entry(msg, &msg_list, list)
		dbg("sequence %d in queue", msg->seqnum);
#endif
}

static void msg_dump(struct hotplug_msg *msg)
{
	dbg("sequence %d, '%s', '%s', '%s'",
	    msg->seqnum, msg->action, msg->devpath, msg->subsystem);
}

static struct hotplug_msg *msg_create(void)
{
	struct hotplug_msg *new_msg;

	new_msg = malloc(sizeof(struct hotplug_msg));
	if (new_msg == NULL)
		dbg("error malloc");
	return new_msg;
}

static void run_queue_delete(struct hotplug_msg *msg)
{
	list_del(&msg->list);
	free(msg);
}

/* orders the message in the queue by sequence number */
static void msg_queue_insert(struct hotplug_msg *msg)
{
	struct hotplug_msg *loop_msg;

	/* sort message by sequence number into list. events
	 * will tend to come in order, so scan the list backwards
	 */
	list_for_each_entry_reverse(loop_msg, &msg_list, list)
		if (loop_msg->seqnum < msg->seqnum)
			break;
	list_add(&msg->list, &loop_msg->list);
	dbg("queued message seq %d", msg->seqnum);

	/* store timestamp of queuing */
	msg->queue_time = time(NULL);

	/* run msg queue manager */
	run_msg_q = 1;

	return ;
}

/* forks event and removes event from run queue when finished */
static void udev_run(struct hotplug_msg *msg)
{
	pid_t pid;
	char action[ACTION_SIZE];
	char devpath[DEVPATH_SIZE];
	char *env[] = { action, devpath, NULL };

	snprintf(action, sizeof(action), "ACTION=%s", msg->action);
	snprintf(devpath, sizeof(devpath), "DEVPATH=%s", msg->devpath);

	pid = fork();
	switch (pid) {
	case 0:
		/* child */
		execle(UDEV_BIN, "udev", msg->subsystem, NULL, env);
		dbg("exec of child failed");
		exit(1);
		break;
	case -1:
		dbg("fork of child failed");
		run_queue_delete(msg);
		/* note: we never managed to run, so we had no impact on 
		 * running_with_devpath(), so don't bother setting run_exec_q
		 */
		break;
	default:
		/* get SIGCHLD in main loop */
		dbg("==> exec seq %d [%d] working at '%s'", msg->seqnum, pid, msg->devpath);
		msg->pid = pid;
	}
}

/* returns already running task with devpath */
static struct hotplug_msg *running_with_devpath(struct hotplug_msg *msg)
{
	struct hotplug_msg *loop_msg;
	list_for_each_entry(loop_msg, &running_list, list)
		if (strncmp(loop_msg->devpath, msg->devpath, sizeof(loop_msg->devpath)) == 0)
			return loop_msg;
	return NULL;
}

/* exec queue management routine executes the events and delays events for the same devpath */
static void exec_queue_manager()
{
	struct hotplug_msg *loop_msg;
	struct hotplug_msg *tmp_msg;
	struct hotplug_msg *msg;

	list_for_each_entry_safe(loop_msg, tmp_msg, &exec_list, list) {
		msg = running_with_devpath(loop_msg);
		if (!msg) {
			/* move event to run list */
			list_move_tail(&loop_msg->list, &running_list);
			udev_run(loop_msg);
			dbg("moved seq %d to running list", loop_msg->seqnum);
		} else {
			dbg("delay seq %d, cause seq %d already working on '%s'",
				loop_msg->seqnum, msg->seqnum, msg->devpath);
		}
	}
}

static void msg_move_exec(struct hotplug_msg *msg)
{
	list_move_tail(&msg->list, &exec_list);
	run_exec_q = 1;
	expected_seqnum = msg->seqnum+1;
	dbg("moved seq %d to exec, next expected is %d",
		msg->seqnum, expected_seqnum);
}

/* msg queue management routine handles the timeouts and dispatches the events */
static void msg_queue_manager()
{
	struct hotplug_msg *loop_msg;
	struct hotplug_msg *tmp_msg;
	time_t msg_age = 0;

	dbg("msg queue manager, next expected is %d", expected_seqnum);
recheck:
	list_for_each_entry_safe(loop_msg, tmp_msg, &msg_list, list) {
		/* move event with expected sequence to the exec list */
		if (loop_msg->seqnum == expected_seqnum) {
			msg_move_exec(loop_msg);
			continue;
		}

		/* move event with expired timeout to the exec list */
		msg_age = time(NULL) - loop_msg->queue_time;
		if (msg_age > EVENT_TIMEOUT_SEC-1) {
			msg_move_exec(loop_msg);
			goto recheck;
		} else {
			break;
		}
	}

	msg_dump_queue();

	if (list_empty(&msg_list) == 0) {
		/* set timeout for remaining queued events */
		struct itimerval itv = {{0, 0}, {EVENT_TIMEOUT_SEC - msg_age, 0}};
		dbg("next event expires in %li seconds",
		    EVENT_TIMEOUT_SEC - msg_age);
		setitimer(ITIMER_REAL, &itv, 0);
	}
}

/* receive the msg, do some basic sanity checks, and queue it */
static void handle_msg(int sock)
{
	struct hotplug_msg *msg;
	int retval;
	struct msghdr smsg;
	struct cmsghdr *cmsg;
	struct iovec iov;
	struct ucred *cred;
	char cred_msg[CMSG_SPACE(sizeof(struct ucred))];

	msg = msg_create();
	if (msg == NULL) {
		dbg("unable to store message");
		return;
	}

	iov.iov_base = msg;
	iov.iov_len = sizeof(struct hotplug_msg);

	memset(&smsg, 0x00, sizeof(struct msghdr));
	smsg.msg_iov = &iov;
	smsg.msg_iovlen = 1;
	smsg.msg_control = cred_msg;
	smsg.msg_controllen = sizeof(cred_msg);

	retval = recvmsg(sock, &smsg, 0);
	if (retval <  0) {
		if (errno != EINTR)
			dbg("unable to receive message");
		return;
	}
	cmsg = CMSG_FIRSTHDR(&smsg);
	cred = (struct ucred *) CMSG_DATA(cmsg);

	if (cmsg == NULL || cmsg->cmsg_type != SCM_CREDENTIALS) {
		dbg("no sender credentials received, message ignored");
		goto skip;
	}

	if (cred->uid != 0) {
		dbg("sender uid=%i, message ignored", cred->uid);
		goto skip;
	}

	if (strncmp(msg->magic, UDEV_MAGIC, sizeof(UDEV_MAGIC)) != 0 ) {
		dbg("message magic '%s' doesn't match, ignore it", msg->magic);
		goto skip;
	}

	/* if no seqnum is given, we move straight to exec queue */
	if (msg->seqnum == -1) {
		list_add(&msg->list, &exec_list);
		run_exec_q = 1;
	} else {
		msg_queue_insert(msg);
	}
	return;

skip:
	free(msg);
	return;
}

static void sig_handler(int signum)
{
	int rc;
	switch (signum) {
		case SIGINT:
		case SIGTERM:
			exit(20 + signum);
			break;
		case SIGALRM:
			/* set flag, then write to pipe if needed */
			run_msg_q = 1;
			goto do_write;
			break;
		case SIGCHLD:
			/* set flag, then write to pipe if needed */
			children_waiting = 1;
			goto do_write;
			break;
		default:
			dbg("unhandled signal");
			return;
	}
	
do_write:
	/* if pipe is empty, write to pipe to force select to return
	 * immediately when it gets called
	 */
	if (!sig_flag) {
		rc = write(pipefds[1],&signum,sizeof(signum));
		if (rc < 0)
			dbg("unable to write to pipe");
		else
			sig_flag = 1;
	}
}

static void udev_done(int pid)
{
	/* find msg associated with pid and delete it */
	struct hotplug_msg *msg;

	list_for_each_entry(msg, &running_list, list) {
		if (msg->pid == pid) {
			dbg("<== exec seq %d came back", msg->seqnum);
			run_queue_delete(msg);
			
			/* we want to run the exec queue manager since there may
			 * be events waiting with the devpath of the one that
			 * just finished
			 */
			run_exec_q = 1;
			return;
		}
	}
}

static void reap_kids()
{
	/* reap all dead children */
	while(1) {
		int pid = waitpid(-1, 0, WNOHANG);
		if ((pid == -1) || (pid == 0))
			break;
		udev_done(pid);
	}
}

/* just read everything from the pipe and clear the flag,
 * the useful flags were set in the signal handler
 */
static void user_sighandler()
{
	int sig;
	while(1) {
		int rc = read(pipefds[0],&sig,sizeof(sig));
		if (rc < 0)
			break;

		sig_flag = 0;
	}
}


int main(int argc, char *argv[])
{
	int ssock, maxsockplus;
	struct sockaddr_un saddr;
	socklen_t addrlen;
	int retval;
	const int on = 1;
	struct sigaction act;
	fd_set readfds;

	init_logging("udevd");
	dbg("version %s", UDEV_VERSION);

	if (getuid() != 0) {
		dbg("need to be root, exit");
		exit(1);
	}
	
	/* setup signal handler pipe */
   retval = pipe(pipefds);
   if (retval < 0) {
      dbg("error getting pipes: %s", strerror(errno));
      exit(1);
   }
	
   retval = fcntl(pipefds[0], F_SETFL, O_NONBLOCK);
   if (retval < 0) {
      dbg("fcntl on read pipe: %s", strerror(errno));
      exit(1);
   }
   
   retval = fcntl(pipefds[1], F_SETFL, O_NONBLOCK);
   if (retval < 0) {
      dbg("fcntl on write pipe: %s", strerror(errno));
      exit(1);
   }

	/* set signal handlers */
	act.sa_handler = sig_handler;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_RESTART;
	sigaction(SIGINT, &act, NULL);
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGALRM, &act, NULL);
	sigaction(SIGCHLD, &act, NULL);

	memset(&saddr, 0x00, sizeof(saddr));
	saddr.sun_family = AF_LOCAL;
	/* use abstract namespace for socket path */
	strcpy(&saddr.sun_path[1], UDEVD_SOCK_PATH);
	addrlen = offsetof(struct sockaddr_un, sun_path) + strlen(saddr.sun_path+1) + 1;

	ssock = socket(AF_LOCAL, SOCK_DGRAM, 0);
	if (ssock == -1) {
		dbg("error getting socket, exit");
		exit(1);
	}

	/* the bind takes care of ensuring only one copy running */
	retval = bind(ssock, (struct sockaddr *) &saddr, addrlen);
	if (retval < 0) {
		dbg("bind failed, exit");
		goto exit;
	}

	/* enable receiving of the sender credentials */
	setsockopt(ssock, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

   FD_ZERO(&readfds);
   FD_SET(ssock, &readfds);
   FD_SET(pipefds[0], &readfds);
	maxsockplus = ssock+1;
	while (1) {
		fd_set workreadfds = readfds;
		retval = select(maxsockplus, &workreadfds, NULL, NULL, NULL);
		
		if (retval < 0) {
			dbg("error in select: %s", strerror(errno));
			continue;
		}
		
		if (FD_ISSET(ssock, &workreadfds))
			handle_msg(ssock);
		
		if (FD_ISSET(pipefds[0], &workreadfds))
			user_sighandler();
		
		if (children_waiting) {
			children_waiting = 0;
			reap_kids();
		}
		
		if (run_msg_q) {
			run_msg_q = 0;
			msg_queue_manager();
		}
		
		if (run_exec_q) {
			
			/* this is tricky.  exec_queue_manager() loops over exec_list, and
			 * calls running_with_devpath(), which loops over running_list. This gives
			 * O(N*M), which can get *nasty*.  Clean up running_list before
			 * calling exec_queue_manager().
			 */
			
			if (children_waiting) {
				children_waiting = 0;
				reap_kids();
			}

			run_exec_q = 0;
			exec_queue_manager();
		}
	}
exit:
	close(ssock);
	exit(1);
}
