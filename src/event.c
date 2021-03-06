/*-
 * Copyright (c) 2011-2014 Baptiste Daroussin <bapt@FreeBSD.org>
 * Copyright (c) 2011-2012 Julien Laffaye <jlaffaye@FreeBSD.org>
 * Copyright (c) 2011 Will Andrews <will@FreeBSD.org>
 * Copyright (c) 2011-2012 Marin Atanasov Nikolov <dnaeon@gmail.com>
 * Copyright (c) 2014 Vsevolod Stakhov <vsevolod@FreeBSD.org>
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer
 *    in this position and unchanged.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "pkg_config.h"
#endif

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/socket.h>

#ifdef HAVE_CAPSICUM
#include <sys/capability.h>
#endif

#include <err.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <libutil.h>

#include "pkg.h"
#include "pkgcli.h"

#define STALL_TIME 5

struct sbuf *messages = NULL;

static char *progress_message = NULL;
static struct sbuf *msg_buf = NULL;
static int last_progress_percent = -1;
static bool progress_alarm = false;
static bool progress_started = false;
static bool progress_debit = false;
static int64_t last_tick = 0;
static int64_t stalled;
static int64_t bytes_per_second;
static int64_t last_update;
static time_t begin = 0;

/* units for format_size */
static const char *unit_SI[] = { " ", "k", "M", "G", "T", };
static const char *unit_IEC[] = { "  ", "Ki", "Mi", "Gi", "Ti", };

static void
format_rate_IEC(char *buf, int size, off_t bytes)
{
	int i;

	bytes *= 100;
	for (i = 0; bytes >= 100*1000 && unit_IEC[i][0] != 'T'; i++)
		bytes = (bytes + 512) / 1024;
	if (i == 0) {
		i++;
		bytes = (bytes + 512) / 1024;
	}
	snprintf(buf, size, "%3lld.%1lld%s%s",
	    (long long) (bytes + 5) / 100,
	    (long long) (bytes + 5) / 10 % 10,
	    unit_IEC[i],
	    i ? "B" : " ");
}

static void
format_size_IEC(char *buf, int size, off_t bytes)
{
	int i;

	for (i = 0; bytes >= 10000 && unit_IEC[i][0] != 'T'; i++)
		bytes = (bytes + 512) / 1024;
	snprintf(buf, size, "%4lld%s%s",
	    (long long) bytes,
	    unit_IEC[i],
	    i ? "B" : " ");
}

static void
format_rate_SI(char *buf, int size, off_t bytes)
{
        int i;

        bytes *= 100;
        for (i = 0; bytes >= 100*1000 && unit_SI[i][0] != 'T'; i++)
                bytes = (bytes + 500) / 1000;
        if (i == 0) {
                i++;
                bytes = (bytes + 500) / 1000;
        }
        snprintf(buf, size, "%3lld.%1lld%s%s",
            (long long) (bytes + 5) / 100,
            (long long) (bytes + 5) / 10 % 10,
            unit_SI[i],
            i ? "B" : " ");
}

static void
format_size_SI(char *buf, int size, off_t bytes)
{
        int i;

        for (i = 0; bytes >= 10000 && unit_SI[i][0] != 'T'; i++)
                bytes = (bytes + 500) / 1000;
        snprintf(buf, size, "%4lld%s%s",
            (long long) bytes,
            unit_SI[i],
            i ? "B" : " ");
}

static void
print_status_end(struct sbuf *msg)
{
	sbuf_finish(msg);
	printf("%s\n", sbuf_data(msg));
	/*printf("\033]0; %s\007", sbuf_data(msg));*/
	sbuf_delete(msg);
}

static void
print_status_begin(struct sbuf *msg)
{
	sbuf_clear(msg_buf);
#ifdef HAVE_LIBJAIL
	static char hostname[MAXHOSTNAMELEN] = "";
	static int jailed = -1;
	size_t intlen;

	if (jailed == -1) {
		intlen = sizeof(jailed);
		if (sysctlbyname("security.jail.jailed", &jailed, &intlen,
		    NULL, 0) == -1)
			jailed = 0;
	}

	if (jailed == 1) {
		if (hostname[0] == '\0')
			gethostname(hostname, sizeof(hostname));

		sbuf_printf(msg, "[%s] ", hostname);
	}
#endif

	if (nbactions > 0 && nbdone > 0)
		sbuf_printf(msg, "[%d/%d] ", nbdone, nbactions);
}

static int
event_sandboxed_call(pkg_sandbox_cb func, int fd, void *ud)
{
	pid_t pid;
	int	status, ret;

	pid = fork();

	switch(pid) {
	case -1:
		warn("fork failed");
		return (EPKG_FATAL);
		break;
	case 0:
		break;
	default:
		/* Parent process */
		while (waitpid(pid, &status, 0) == -1) {
			if (errno != EINTR) {
				warn("Sandboxed process pid=%d", (int)pid);
				ret = -1;
				break;
			}
		}

		if (WIFEXITED(status)) {
			ret = WEXITSTATUS(status);
		}
		if (WIFSIGNALED(status)) {
			/* Process got some terminating signal, hence stop the loop */
			fprintf(stderr, "Sandboxed process pid=%d terminated abnormally by signal: %d\n",
					(int)pid, WTERMSIG(status));
			ret = -1;
		}
		return (ret);
	}

	/* Here comes child process */
#ifdef HAVE_CAPSICUM
	if (cap_enter() < 0 && errno != ENOSYS) {
		warn("cap_enter() failed");
		return (EPKG_FATAL);
	}
#endif

	/*
	 * XXX: if capsicum is not enabled we basically have no idea of how to
	 * make a sandbox
	 */
	ret = func(fd, ud);

	_exit(ret);
}

static int
event_sandboxed_get_string(pkg_sandbox_cb func, char **result, int64_t *len,
		void *ud)
{
	pid_t pid;
	int	status, ret = EPKG_OK;
	int pair[2], r, allocated_len = 0, off = 0;
	char *buf = NULL;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == -1) {
		warn("socketpair failed");
		return (EPKG_FATAL);
	}

	pid = fork();

	switch(pid) {
	case -1:
		warn("fork failed");
		return (EPKG_FATAL);
		break;
	case 0:
		break;
	default:
		/* Parent process */
		close(pair[0]);
		/*
		 * We use blocking IO here as if the child is terminated we would have
		 * EINTR here
		 */
		buf = malloc(BUFSIZ);
		if (buf == NULL) {
			warn("malloc failed");
			return (EPKG_FATAL);
		}
		allocated_len = BUFSIZ;
		do {
			if (off >= allocated_len) {
				allocated_len *= 2;
				buf = realloc(buf, allocated_len);
				if (buf == NULL) {
					warn("realloc failed");
					return (EPKG_FATAL);
				}
			}

			r = read(pair[1], buf + off, allocated_len - off);
			if (r == -1 && errno != EINTR) {
				free(buf);
				warn("read failed");
				return (EPKG_FATAL);
			}
			else if (r > 0) {
				off += r;
			}
		} while (r > 0);

		/* Fill the result buffer */
		*len = off;
		*result = buf;
		if (*result == NULL) {
			warn("malloc failed");
			kill(pid, SIGTERM);
			ret = EPKG_FATAL;
		}
		while (waitpid(pid, &status, 0) == -1) {
			if (errno != EINTR) {
				warn("Sandboxed process pid=%d", (int)pid);
				ret = -1;
				break;
			}
		}

		if (WIFEXITED(status)) {
			ret = WEXITSTATUS(status);
		}
		if (WIFSIGNALED(status)) {
			/* Process got some terminating signal, hence stop the loop */
			fprintf(stderr, "Sandboxed process pid=%d terminated abnormally by signal: %d\n",
					(int)pid, WTERMSIG(status));
			ret = -1;
		}
		return (ret);
	}

	/* Here comes child process */
	close(pair[1]);

#ifdef HAVE_CAPSICUM
	if (cap_enter() < 0 && errno != ENOSYS) {
		warn("cap_enter() failed");
		return (EPKG_FATAL);
	}
#endif

	/*
	 * XXX: if capsicum is not enabled we basically have no idea of how to
	 * make a sandbox
	 */
	ret = func(pair[0], ud);

	close(pair[0]);

	_exit(ret);
}

static void
progress_alarm_handler(int signo)
{
	if (progress_alarm && progress_started) {
		last_progress_percent = -1;
		alarm(1);
	}
}

static void
stop_progressbar(void)
{
	if (progress_started) {
		if (!isatty(STDOUT_FILENO))
			printf(" done");
		putchar('\n');
	}

	last_progress_percent = -1;
	progress_alarm = false;
	progress_started = false;
}

static void
draw_progressbar(int64_t current, int64_t total)
{
	int percent;
	int64_t transferred;
	time_t now;
	char buf[7];
	int64_t bytes_left;
	int cur_speed;
	int64_t elapsed;
	int hours, minutes, seconds;
	int r = 0;

	percent = (total != 0) ? (current * 100. / total) : 100;

	if (progress_started && (percent != last_progress_percent || current == total)) {
		last_progress_percent = percent;

		r = printf("\r%s: %d%%", progress_message, percent);
		if (progress_debit) {
			if (total > current) {
				now = time(NULL);
				transferred = current - last_tick;
				last_tick = current;
				bytes_left = total - current;
				if (bytes_left > 0) {
					elapsed = (now > last_update) ? now - last_update : 0;
				} else {
					elapsed = now - begin;
				}

				if (elapsed != 0)
					cur_speed = (transferred / elapsed);
				else
					cur_speed = transferred;

#define AGE_FACTOR 0.9
				if (bytes_per_second != 0) {
					bytes_per_second = (bytes_per_second * AGE_FACTOR) +
									(cur_speed * (1.0 - AGE_FACTOR));
				} else {
					bytes_per_second = cur_speed;
				}

				humanize_number(buf, sizeof(buf),
					current,"B", HN_AUTOSCALE, 0);
				printf(" %*s", (int)sizeof(buf), buf);

				format_rate_SI(buf, sizeof(buf), transferred);
				printf(" %s/s ", buf);

				if (!transferred)
					stalled += elapsed;
				else
					stalled = 0;

				if (stalled >= STALL_TIME)
					printf(" - stalled -");
				else if (bytes_per_second == 0 && bytes_left)
					printf("   --:-- ETA");
				else {
					if (bytes_left > 0)
						seconds = bytes_left / bytes_per_second;
					else
						seconds = elapsed;

					hours = seconds / 3600;
					seconds -= hours * 3600;
					minutes = seconds / 60;
					seconds -= minutes * 60;

					if (hours != 0)
						printf("%02d:%02d:%0d", hours, minutes, seconds);
					else
						printf("   %02d:%02d", minutes, seconds);

					if (bytes_left > 0)
						printf(" ETA");
					else
						printf("    ");
				}
			}
			else {
				struct winsize winsize;
				int ttywidth = 80;


				/* Bad hack to erase string */
				if (ioctl(fileno(stdout), TIOCGWINSZ, &winsize) != -1 &&
								winsize.ws_col != 0)
					ttywidth = winsize.ws_col;
				else
					ttywidth = 80;

				humanize_number(buf, sizeof(buf),
					current,"B", HN_AUTOSCALE, 0);
				if (ttywidth > r + sizeof(buf))
					printf(" of %-*s", (int)(ttywidth - r - strlen(buf)), buf);
			}
		}
		fflush(stdout);
	}
	if (current >= total) {
		stop_progressbar();
	}
	else if (!progress_alarm && progress_started) {
		/* Setup auxiliary alarm */
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = progress_alarm_handler;
		sa.sa_flags = SA_RESTART;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGALRM, &sa, NULL);
		alarm(1);
		progress_alarm = true;
	}
}

int
event_callback(void *data, struct pkg_event *ev)
{
	struct pkg *pkg = NULL, *pkg_new, *pkg_old;
	int *debug = data;
	struct pkg_event_conflict *cur_conflict;
	const char *filename;

	if (msg_buf == NULL) {
		msg_buf = sbuf_new_auto();
	}

	/*
	 * If a progressbar has been interrupted by another event, then
	 * we need to stop it immediately to avoid bad formatting
	 */
	if (progress_started && ev->type != PKG_EVENT_PROGRESS_TICK) {
		stop_progressbar();
		progress_started = true;
	}

	switch(ev->type) {
	case PKG_EVENT_ERRNO:
		warnx("%s(%s): %s", ev->e_errno.func, ev->e_errno.arg,
		    strerror(ev->e_errno.no));
		break;
	case PKG_EVENT_ERROR:
		warnx("%s", ev->e_pkg_error.msg);
		break;
	case PKG_EVENT_NOTICE:
		if (!quiet)
			warnx("%s", ev->e_pkg_notice.msg);
		break;
	case PKG_EVENT_DEVELOPER_MODE:
		warnx("DEVELOPER_MODE: %s", ev->e_pkg_error.msg);
		break;
	case PKG_EVENT_UPDATE_ADD:
		if (quiet || !isatty(STDOUT_FILENO))
			break;
		printf("\rPushing new entries %d/%d", ev->e_upd_add.done, ev->e_upd_add.total);
		if (ev->e_upd_add.total == ev->e_upd_add.done)
			printf("\n");
		break;
	case PKG_EVENT_UPDATE_REMOVE:
		if (quiet || !isatty(STDOUT_FILENO))
			break;
		printf("\rRemoving entries %d/%d", ev->e_upd_remove.done, ev->e_upd_remove.total);
		if (ev->e_upd_remove.total == ev->e_upd_remove.done)
			printf("\n");
		break;
	case PKG_EVENT_FETCH_BEGIN:
		if (quiet)
			break;
		filename = strrchr(ev->e_fetching.url, '/');
		if (filename != NULL) {
			filename++;
		} else {
			/*
			 * We failed at being smart, so display
			 * the entire url.
			 */
			filename = ev->e_fetching.url;
		}
		print_status_begin(msg_buf);
		progress_debit = true;
		sbuf_printf(msg_buf, "Fetching %s", filename);

		break;
	case PKG_EVENT_FETCH_FINISHED:
		progress_debit = false;
		break;
	case PKG_EVENT_INSTALL_BEGIN:
		if (quiet)
			break;
		else {
			nbdone++;
			print_status_begin(msg_buf);

			pkg = ev->e_install_begin.pkg;
			pkg_sbuf_printf(msg_buf, "Installing %n-%v", pkg, pkg);
		}
		break;
	case PKG_EVENT_INSTALL_FINISHED:
		if (quiet)
			break;
		if (pkg_has_message(ev->e_install_finished.pkg)) {
			if (messages == NULL)
				messages = sbuf_new_auto();
			pkg_sbuf_printf(messages, "Message for %n-%v:\n %M\n",
			    ev->e_install_finished.pkg,
			    ev->e_install_finished.pkg,
			    ev->e_install_finished.pkg);
		}
		break;
	case PKG_EVENT_INTEGRITYCHECK_BEGIN:
		if (quiet)
			break;
		printf("Checking integrity...");
		break;
	case PKG_EVENT_INTEGRITYCHECK_FINISHED:
		if (quiet)
			break;
		printf(" done (%d conflicting)\n", ev->e_integrity_finished.conflicting);
		break;
	case PKG_EVENT_INTEGRITYCHECK_CONFLICT:
		if (*debug == 0)
			break;
		printf("\nConflict found on path %s between %s and ",
		    ev->e_integrity_conflict.pkg_path,
		    ev->e_integrity_conflict.pkg_uid);
		cur_conflict = ev->e_integrity_conflict.conflicts;
		while (cur_conflict) {
			if (cur_conflict->next)
				printf("%s, ", cur_conflict->uid);
			else
				printf("%s", cur_conflict->uid);

			cur_conflict = cur_conflict->next;
		}
		printf("\n");
		break;
	case PKG_EVENT_DEINSTALL_BEGIN:
		if (quiet)
			break;
		nbdone++;

		print_status_begin(msg_buf);

		pkg = ev->e_install_begin.pkg;
		pkg_sbuf_printf(msg_buf, "Deleting %n-%v", pkg, pkg);
		break;
	case PKG_EVENT_DEINSTALL_FINISHED:
		if (quiet)
			break;
		break;
	case PKG_EVENT_UPGRADE_BEGIN:
		if (quiet)
			break;
		pkg_new = ev->e_upgrade_begin.new;
		pkg_old = ev->e_upgrade_begin.old;
		nbdone++;

		print_status_begin(msg_buf);

		switch (pkg_version_change_between(pkg_new, pkg_old)) {
		case PKG_DOWNGRADE:
			pkg_sbuf_printf(msg_buf, "Downgrading %n from %v to %v",
			    pkg_new, pkg_new, pkg_old);
			break;
		case PKG_REINSTALL:
			pkg_sbuf_printf(msg_buf, "Reinstalling %n-%v",
		    pkg_old, pkg_old);
			break;
		case PKG_UPGRADE:
			pkg_sbuf_printf(msg_buf, "Upgrading %n from %v to %v",
			    pkg_new, pkg_old, pkg_new);
			break;
		}
		break;
	case PKG_EVENT_UPGRADE_FINISHED:
		if (quiet)
			break;
		pkg_new = ev->e_upgrade_begin.new;
		if (pkg_has_message(pkg_new)) {
			if (messages == NULL)
				messages = sbuf_new_auto();
			pkg_sbuf_printf(messages, "Message for %n-%v:\n %M\n",
				pkg_new, pkg_new, pkg_new);
		}
		break;
	case PKG_EVENT_LOCKED:
		pkg = ev->e_locked.pkg;
		pkg_printf("\n%n-%v is locked and may not be modified\n", pkg, pkg);
		break;
	case PKG_EVENT_REQUIRED:
		pkg = ev->e_required.pkg;
		pkg_printf("\n%n-%v is required by: %r%{%rn-%rv%| %}", pkg, pkg, pkg);
		if (ev->e_required.force == 1)
			fprintf(stderr, ", deleting anyway\n");
		else
			fprintf(stderr, "\n");
		break;
	case PKG_EVENT_ALREADY_INSTALLED:
		if (quiet)
			break;
		pkg = ev->e_already_installed.pkg;
		pkg_printf("the most recent version of %n-%v is already installed\n",
				pkg, pkg);
		break;
	case PKG_EVENT_NOT_FOUND:
		printf("Package '%s' was not found in "
		    "the repositories\n", ev->e_not_found.pkg_name);
		break;
	case PKG_EVENT_MISSING_DEP:
		fprintf(stderr, "missing dependency %s-%s\n",
		    pkg_dep_name(ev->e_missing_dep.dep),
		    pkg_dep_version(ev->e_missing_dep.dep));
		break;
	case PKG_EVENT_NOREMOTEDB:
		fprintf(stderr, "Unable to open remote database \"%s\". "
		    "Try running '%s update' first.\n", ev->e_remotedb.repo,
		    getprogname());
		break;
	case PKG_EVENT_NOLOCALDB:
		fprintf(stderr, "Local package database nonexistent!\n");
		break;
	case PKG_EVENT_NEWPKGVERSION:
		newpkgversion = true;
		printf("New version of pkg detected; it needs to be "
		    "installed first.\n");
		break;
	case PKG_EVENT_FILE_MISMATCH:
		pkg = ev->e_file_mismatch.pkg;
		pkg_fprintf(stderr, "%n-%v: checksum mismatch for %S\n", pkg,
		    pkg, pkg_file_path(ev->e_file_mismatch.file));
		break;
	case PKG_EVENT_PLUGIN_ERRNO:
		warnx("%s: %s(%s): %s",
		    pkg_plugin_get(ev->e_plugin_errno.plugin, PKG_PLUGIN_NAME),
		    ev->e_plugin_errno.func, ev->e_plugin_errno.arg,
		    strerror(ev->e_plugin_errno.no));
		break;
	case PKG_EVENT_PLUGIN_ERROR:
		warnx("%s: %s",
		    pkg_plugin_get(ev->e_plugin_error.plugin, PKG_PLUGIN_NAME),
		    ev->e_plugin_error.msg);
		break;
	case PKG_EVENT_PLUGIN_INFO:
		if (quiet)
			break;
		printf("%s: %s\n",
		    pkg_plugin_get(ev->e_plugin_info.plugin, PKG_PLUGIN_NAME),
		    ev->e_plugin_info.msg);
		break;
	case PKG_EVENT_INCREMENTAL_UPDATE:
		if (!quiet)
			printf("Incremental update completed, %d packages "
			    "processed:\n"
			    "%d packages updated, %d removed and %d added.\n",
			    ev->e_incremental_update.processed,
			    ev->e_incremental_update.updated,
			    ev->e_incremental_update.removed,
			    ev->e_incremental_update.added);
		break;
	case PKG_EVENT_DEBUG:
		fprintf(stderr, "DBG(%d)[%d]> %s\n", ev->e_debug.level,
			(int)getpid(), ev->e_debug.msg);
		break;
	case PKG_EVENT_QUERY_YESNO:
		return ( ev->e_query_yesno.deft ?
			query_yesno(true, ev->e_query_yesno.msg, "[Y/n]") :
			query_yesno(false, ev->e_query_yesno.msg, "[y/N]") );
		break;
	case PKG_EVENT_QUERY_SELECT:
		return query_select(ev->e_query_select.msg, ev->e_query_select.items,
			ev->e_query_select.ncnt, ev->e_query_select.deft);
		break;
	case PKG_EVENT_SANDBOX_CALL:
		return ( event_sandboxed_call(ev->e_sandbox_call.call,
				ev->e_sandbox_call.fd,
				ev->e_sandbox_call.userdata) );
		break;
	case PKG_EVENT_SANDBOX_GET_STRING:
		return ( event_sandboxed_get_string(ev->e_sandbox_call_str.call,
				ev->e_sandbox_call_str.result,
				ev->e_sandbox_call_str.len,
				ev->e_sandbox_call_str.userdata) );
		break;
	case PKG_EVENT_PROGRESS_START:
		if (progress_message != NULL) {
			free(progress_message);
			progress_message = NULL;
		}
		if (!quiet) {
			if (ev->e_progress_start.msg != NULL) {
				progress_message = strdup(ev->e_progress_start.msg);
			} else {
				sbuf_finish(msg_buf);
				progress_message = strdup(sbuf_data(msg_buf));
			}
			last_progress_percent = -1;
			last_tick = 0;
			begin = last_update = time(NULL);
			bytes_per_second = 0;

			progress_started = true;
			if (!isatty(STDOUT_FILENO))
				printf("%s...", progress_message);
		}
		break;
	case PKG_EVENT_PROGRESS_TICK:
		if (quiet)
			break;
		if (isatty(STDOUT_FILENO))
			draw_progressbar(ev->e_progress_tick.current, ev->e_progress_tick.total);
		else if (progress_started && ev->e_progress_tick.current >=
		    ev->e_progress_tick.total)
			stop_progressbar();
		break;
	case PKG_EVENT_BACKUP:
		sbuf_cat(msg_buf, "Backing up");
		sbuf_finish(msg_buf);
		break;
	case PKG_EVENT_RESTORE:
		sbuf_cat(msg_buf, "Restoring");
		sbuf_finish(msg_buf);
		break;
	default:
		break;
	}

	return 0;
}
