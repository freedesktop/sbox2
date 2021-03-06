/* sb2-monitor:
 *
 * This is used to wrap the commands started by the "sb2" script, 
 * wait for results, and then start a log analyzer to print out 
 * statistics after the command has finished.
 *
 * Basically, this starts the command as a child process,
 * waits until the command completes (relaying signals to the child)
 * and finally calls an external script.
 *
 * In practise, it is easy to say that this "relays signals" but 
 * implementing that requires some tricks - see comments below.
 * Also note that a signal proxy is not possible with 100%
 * accuracy due to process groups and other oddities that have been
 * "built in" to the signal system. All of this can be described as 
 * yet another best-effort game..
 *
 *
 * Copyright (c) 2008 Nokia Corporation. All rights reserved.
 * Author: Lauri T. Aarnio
 * Licensed under LGPL version 2.1, see top level LICENSE file for details.
*/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include <config.h>
#include <config_hardcoded.h>

#ifdef __APPLE__
 #include <signal.h>
#endif

static pid_t	child_pid;
static pid_t	original_process_group;
static pid_t	new_process_group;
static int	debug = 0;

static const char *progname;

#define DEBUG_MSG(...) \
	do { \
		if (debug) { \
			fprintf(stderr, "%s [pid=%d]:", progname, getpid()); \
			fprintf(stderr, __VA_ARGS__); \
		} \
	} while(0)

static void usage_exit(const char *errmsg, int exitstatus)
{
	if (errmsg) 
		fprintf(stderr, "%s: Error: %s\n", progname, errmsg);

	fprintf(stderr, 
		"\n%s: Usage:\n"
		"\t%s [options_for_%s] -- command [parameters]\n"
		"\nOptions:\n"
		"\t-x program\tExecute 'program' after 'command' terminates\n"
		"\t-d\tEnable debug messages\n"
		"\nExample:\n"
		"\t%s -x /bin/echo -- signaltester -n 5\n",
		progname, progname, progname, progname);

	exit(exitstatus);
}

/* Signal handler, which relays the signal sent by kill() or sigqueue()
 * to the child process.
 *
 * this is a bit tricky since we are trying to act as an accurate 
 * signal bridge: There are some signals that are usually generated
 * by an execeptional condition (like SIGILL, SIGFPE and SIGSEGV),
 * signals that originate from timers, etc.
 * But it is still possible that these signal numbers are sent by another
 * user process - although not recommended, it is possible to recycle
 * those those signal numbers for something else -
 * so we'll have to check how the signal was sent before deciding
 * what to do.
*/
static void signal_handler(int sig, siginfo_t *info, void *ptr)
{
	(void)ptr; /* unused param */

	DEBUG_MSG("Got signal %d\n", sig);

	switch (sig) {
	case SIGILL: case SIGFPE: case SIGSEGV:
		if ((info->si_code != SI_USER) &&
		   (info->si_code != SI_QUEUE)) {
			/* OOPS, the signal is a real, fatal, deadly 
			 * that must not be ignored. */
			DEBUG_MSG("Deadly signal\n");
			exit(250);
		}
		/* else it is user-generated signal */
		break;

	case SIGCHLD:
		if (info->si_code == CLD_STOPPED) {
			/* Job control needs special processing. Here the
			 * child has been stopped - stop ourselves, too.
			 * Now this is where things really get complicated:
			 * when the child process needs to continue, it
			 * it usually waked with SIGCONT, which gets sent
			 * to the process group. But this wrapper is currently
			 * in a different process group...so process groups
			 * must be changed back to the original value before
			 * stopping ourselves..the story continues below..
			*/
			setpgid(0, original_process_group);
			
			raise(SIGSTOP);
			return;
		}
		break;
	
	case SIGCONT:
		/* ...need to continue. When this process was stopped,
		 * the process group was changed (above) and now it needs
		 * to be changed again away from the process group that
		 * is used by child_pid.
		*/
		setpgid(0, new_process_group);
		break;
	}

	if (info->si_code == SI_QUEUE) {
		DEBUG_MSG("signal %d => sending it to %d by sigqueue\n", 
			sig, (int)child_pid);
#ifndef __APPLE__
		sigqueue(child_pid, sig, info->si_value);
#else
		kill(child_pid, sig);
#endif
	} else if (info->si_code == SI_USER) {
		DEBUG_MSG("signal %d => sending it to %d by kill\n", 
			sig, (int)child_pid);
		kill(child_pid, sig);
	} else {
		DEBUG_MSG("signal %d: si_code is %d, ignored\n", 
			sig, info->si_code);
	}
}

static void initialize_signal_handler(int sig)
{
	struct	sigaction	act;

	memset(&act, 0, sizeof(act));

	act.sa_sigaction = signal_handler;
	act.sa_flags = SA_SIGINFO;

	/* block all other signals while this signal is being processed
	 * (to avoid accidental reordering of signals)
	*/
	sigfillset(&act.sa_mask);

	sigaction(sig, &act, NULL);
}

/* Set up signal handlers for all signals that can be caught. */
static void catch_all_signals(void)
{
	/* order here tries to follows the numerical order of signals 
	 * on Linux (see signal(7)) and Mac OS X (see signal(2))
	*/
	initialize_signal_handler(SIGHUP); /* 1 */
	initialize_signal_handler(SIGINT);
	initialize_signal_handler(SIGQUIT);
	initialize_signal_handler(SIGILL);
	initialize_signal_handler(SIGTRAP); /* 5 */
	initialize_signal_handler(SIGABRT);
#ifdef SIGIOT /* old name for SIGABRT */
	initialize_signal_handler(SIGIOT);
#endif
#ifdef SIGEMT
	initialize_signal_handler(SIGEMT);
#endif
	initialize_signal_handler(SIGFPE);

	/* SIGKILL, 9, cannot be caught or ignored */

	initialize_signal_handler(SIGBUS);
	initialize_signal_handler(SIGSEGV); /* 11 */
	initialize_signal_handler(SIGSYS);
	initialize_signal_handler(SIGPIPE);
	initialize_signal_handler(SIGALRM);
	initialize_signal_handler(SIGTERM); /* 15 */
	initialize_signal_handler(SIGURG);

	/* SIGSTOP, 17, cannot be caught or ignored */

	initialize_signal_handler(SIGTSTP);
	initialize_signal_handler(SIGCONT);
	initialize_signal_handler(SIGCHLD); /* 20 */
	initialize_signal_handler(SIGTTIN);

	initialize_signal_handler(SIGTTOU);
	initialize_signal_handler(SIGIO);
	initialize_signal_handler(SIGXCPU);
	initialize_signal_handler(SIGXFSZ); /* 25 */
	initialize_signal_handler(SIGVTALRM);
	initialize_signal_handler(SIGPROF);
	initialize_signal_handler(SIGWINCH);
#ifdef SIGPWR
	initialize_signal_handler(SIGPWR);
#endif
#ifdef SIGLOST
	initialize_signal_handler(SIGLOST);
#endif
#ifdef SIGINFO
	initialize_signal_handler(SIGINFO);
#endif
	initialize_signal_handler(SIGUSR1);
	initialize_signal_handler(SIGUSR2);
#ifdef SIGTHR
	initialize_signal_handler(SIGTHR);
#endif
}


int main(int argc, char *argv[])
{
	int	status;
	int	pipe_fds[2];
	char	ch;
	char	*command_to_exec_at_end = NULL;
	int	opt;
	char	*exit_reason;
	char	exit_status[100];
	int	new_stdin;
	char	*sbox_libsb2 = NULL;

	progname = argv[0];
	
	while ((opt = getopt(argc, argv, "L:x:dh")) != -1) {
		switch (opt) {
		case 'L': sbox_libsb2 = optarg; break;
		case 'h': usage_exit(NULL, 0); break;
		case 'd': debug = 1; break;
		case 'x': command_to_exec_at_end = optarg; break;
		default: usage_exit("Illegal option", 1); break;
		}
	}

	if (optind >= argc) 
		usage_exit("Wrong number of parameters", 1);

	original_process_group = getpgrp();

	DEBUG_MSG("PGID=%d\n", (int)getpgrp());

	/* create a child process which will execute the command. */
	child_pid = fork();

	switch (child_pid) {
	case -1: /* fork failed */
		usage_exit("fork failed", 1);
		break;
		
	case 0: /* child - the worker process */
		/* child remains in the original process group.. */
		if (debug) {
			int	i;

			DEBUG_MSG("child started, exec:\n");
			for (i=optind; argv[i]; i++) {
				DEBUG_MSG("[%d]='%s'\n", i, argv[i]);
			}
		}

		/* set LD_PRELOAD, so that the binary started by execvp()
		 * will be running in sb2'ed environment (depending on
		 * the mapping mode & rules, it might not be possible
		 * to execute 'sb2-monitor' in that environment)
		*/
		if (sbox_libsb2) {
			char	*old_ld_preload = getenv("LD_PRELOAD");
			char	*new_ld_preload = NULL;

			if (old_ld_preload) {
				char	*p_libsb2 = strstr(old_ld_preload,
					sbox_libsb2);

				DEBUG_MSG("child: LD_PRELOAD was '%s'\n",
						old_ld_preload);
				if (!p_libsb2) {
					/* LD_PRELOAD is defined, but libsb2
					 * was not included. Add it now. */
					if (asprintf(&new_ld_preload, "%s:%s",
						old_ld_preload, sbox_libsb2) < 0) {
						fprintf(stderr,
							 "%s: asprintf failed\n",
							 progname);
						exit(1);
					}
				} /* else libsb2 seems to be already included */
			} else {
				/* LD_PRELOAD was not set. */
				DEBUG_MSG("child: no previous LD_PRELOAD\n");
				new_ld_preload = sbox_libsb2;
			}
			if (new_ld_preload) {
				/* need to set/modify LD_PRELOAD */
				DEBUG_MSG("child: setting LD_PRELOAD to '%s'\n",
					new_ld_preload);
				setenv("LD_PRELOAD", new_ld_preload, 1);
			}
		} else {
			DEBUG_MSG("child: WARNING: "
				"no '-L lib' option => LD_PRELOAD not set\n");
		}

		execvp(argv[optind], argv+optind);
		DEBUG_MSG("child: exec failed\n");
		exit(1);
	}

	/* parent - the signal relay process */

	/* close unnecessary file descriptors, and reopen stdin and stdout */
	fclose(stdin);	/* fd 0 */
	fclose(stdout);	/* fd 1 */
	/* stderr = fd 2 = left open and duplicated to stdout: */
	dup2(2, 1);
	new_stdin = open("/dev/null", O_RDONLY);
	if (new_stdin < 0) usage_exit("failed to open /dev/null", 1);
	if (new_stdin != 0) dup2(new_stdin, 0);

	/* FIXME: Maybe we should close all fds >= 3 here, but Linux does
	 * not seem to have any easy way to do so (no F_CLOSEM, no closefrom(),
	 * no fdwalk() that are available in other systems (solaris, etc)
	*/

	/* The process group must be changed. 
	 * failing to do so would duplicate all signals sent to the process
	 * group, because there doesn't seem to be any way to detect if
	 * the signal signal was sent to a process group or directly to 
	 * the process itself => this program just forwards all signals..
	 *
	 * Note that the PID of this process is very likely already in use as 
	 * a process group (for example, if "sb2" was started from a shell,
	 * the shell has usually created a new process group using the PID
	 * of the new process), so we'll have to create a new PGID. A safe way
	 * is to create another child process, and use that as the new PGID.
	 *
	 * N.B. When the process group has been changed, this process won't
	 * be able to receive keyboard-generated signals (intr,tstp,..) anymore.
	*/
	if (pipe(pipe_fds) < 0) usage_exit("pipe() failed", 1);
	new_process_group = fork();
	switch (new_process_group) {
	case -1:
		/* ARGH! fork should not fail, not now. 
		 * can't do anything else than fail completely */
		kill(SIGTERM, child_pid);
		usage_exit("fork failed", 1);
		break;

	case 0:
		/* the second child. Sit and wait for input that
		 * never arrives - purpose of the pipe is only to keep
		 * the PID reserved until the parent dies.
		*/
#ifdef __APPLE__
		setpgrp(0, 0); /* change process group to
				* avoid signals*/
#else
		setpgrp();
#endif
		close(pipe_fds[1]); /* close W-end */
		while (read(pipe_fds[0], &ch, 1) > 0);
		DEBUG_MSG("dummy child: done\n");
		exit(0);
	}

	/* close R-end of the pipe. After this there is no need to
	 * do anything considering the second child, it will die away
	 * when the parent process dies and the W-end of the pipe is 
	 * closed automatically.
	*/
	close(pipe_fds[0]); 
		
	setpgid(0, new_process_group); /* finally, change process group! */
	
	/* now catch all signals that are sent to this PID,
	 * but since the process group was just changed, this won't catch
	 * those signals that were sent to that process group being used by
	 * the first child.
	*/
	catch_all_signals();

	errno = 0;

	/* wait until the worker child has finished. */
	while ((waitpid(child_pid, &status, 0) == -1) &&
	       (errno == EINTR)) {
		DEBUG_MSG("parent: EINTR\n");
		errno = 0;
	}

	DEBUG_MSG("parent: child returned\n");

	/* determine reason why it exited, to be forwarded to the
	 * report generator
	*/
	if (WIFEXITED(status)) {
		exit_reason = "exit";
		snprintf(exit_status, sizeof(exit_status), "%d",
			WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		exit_reason = "signal";
		snprintf(exit_status, sizeof(exit_status), "%d%s",
			WTERMSIG(status),
			(WCOREDUMP(status) ? " (core dumped)" : ""));
	} else {
		/* this should not be possible */
		exit_reason = "UNKNOWN";
		*exit_status = '\0';
	}
	DEBUG_MSG("%s %s\n", exit_reason, exit_status);

	if (command_to_exec_at_end) {
		/* time to exec the external script */
		execlp(command_to_exec_at_end, command_to_exec_at_end,
			exit_reason, exit_status, NULL);

		/* OOPS, exec failed */
		DEBUG_MSG("Failed to execute %s\n", command_to_exec_at_end);
		return(1);
	}
	return(0);
}

