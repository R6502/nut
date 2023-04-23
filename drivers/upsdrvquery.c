/* upsdrvquery.c - a single query shot over a driver socket,
                   tracked until a response arrives, returning
                   that line and closing a connection

   Copyright (C) 2023  Jim Klimov <jimklimov+nut@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "config.h"  /* must be the first header */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef WIN32
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#else
#include "wincompat.h"
#endif

#include "common.h"
#include "upsdrvquery.h"
#include "nut_stdint.h"

TYPE_FD upsdrvquery_connect(const char *sockfn) {
	TYPE_FD	sockfd;

	/* Code borrowed from our numerous sock_connect() implems */
#ifndef WIN32
	struct	sockaddr_un sa;
	ssize_t	ret;

	memset(&sa, '\0', sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sockfn);

	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (sockfd < 0) {
		upslog_with_errno(LOG_ERR, "open socket");
		return ERROR_FD;
	}

	if (connect(sockfd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
		upslog_with_errno(LOG_ERR, "connect to driver socket at %s", sockfn);
		close(sockfd);
		return ERROR_FD;
	}

	ret = fcntl(sockfd, F_GETFL, 0);
	if (ret < 0) {
		upslog_with_errno(LOG_ERR, "fcntl get on driver socket %s failed", sockfn);
		close(sockfd);
		return ERROR_FD;
	}

	if (fcntl(sockfd, F_SETFL, ret | O_NDELAY) < 0) {
		upslog_with_errno(LOG_ERR, "fcntl set O_NDELAY on driver socket %s failed", sockfn);
		close(sockfd);
		return ERROR_FD;
	}
#else
	BOOL	result = WaitNamedPipe(sockfn, NMPWAIT_USE_DEFAULT_WAIT);

	if (result == FALSE) {
		upslog_with_errno(LOG_ERR, "WaitNamedPipe : %d\n", GetLastError());
		return ERROR_FD;
	}

	sockfd = CreateFile(
			sockfn,         // pipe name
			GENERIC_READ |  // read and write access
			GENERIC_WRITE,
			0,              // no sharing
			NULL,           // default security attributes FIXME
			OPEN_EXISTING,  // opens existing pipe
			FILE_FLAG_OVERLAPPED, //  enable async IO
			NULL);          // no template file

	if (sockfd == INVALID_HANDLE_VALUE) {
		upslog_with_errno(LOG_ERR, "CreateFile : %d\n", GetLastError());
		return ERROR_FD;
	}
#endif  /* WIN32 */

	return sockfd;
}

TYPE_FD upsdrvquery_connect_drvname_upsname(const char *drvname, const char *upsname) {
	char	pidfn[SMALLBUF];
#ifndef WIN32
	struct stat     fs;
	snprintf(pidfn, sizeof(pidfn), "%s/%s-%s",
		dflt_statepath(), drvname, upsname);
	check_unix_socket_filename(pidfn);
	if (stat(pidfn, &fs)) {
		upslog_with_errno(LOG_ERR, "Can't open %s", pidfn);
		return ERROR_FD;
	}
#else
	snprintf(pidfn, sizeof(pidfn), "\\\\.\\pipe\\%s-%s", drvname, upsname);
#endif  /* WIN32 */

	return upsdrvquery_connect(pidfn);
}

void upsdrvquery_close(TYPE_FD sockfd) {
	if (INVALID_FD(sockfd))
		return;

#ifndef WIN32
	close(sockfd);
#else
	if (DisconnectNamedPipe(sockfd) == 0) {
		upslogx(LOG_ERR,
			"DisconnectNamedPipe error : %d",
			(int)GetLastError());
	}
	CloseHandle(sockfd);
#endif  /* WIN32 */
}

ssize_t upsdrvquery_read_timeout(TYPE_FD sockfd, struct timeval tv, char *buf, const size_t bufsz) {
	ssize_t	ret;

	if (INVALID_FD(sockfd)) {
		upslog_with_errno(LOG_ERR, "socket not initialized");
		return -1;
	}

#ifndef WIN32
	fd_set	rfds;

	FD_ZERO(&rfds);
	FD_SET(sockfd, &rfds);

	if (select(sockfd + 1, &rfds, NULL, NULL, &tv) < 0) {
		upslog_with_errno(LOG_ERR, "select with socket");
		/* close(sockfd); */
		return -1;
	}

	if (!FD_ISSET(sockfd, &rfds)) {
		upsdebugx(5, "%s: received nothing from driver socket", __func__);
		return -2;	/* timed out, no info */
	}

	memset(buf, 0, bufsz);
	ret = read(sockfd, buf, bufsz);
#else
/*
	upslog_with_errno(LOG_ERR, "Support for this platform is not currently implemented");
	return -1;
*/
	DWORD	bytesRead;
	OVERLAPPED	read_overlapped;

	/* FIXME? */
	NUT_UNUSED_VARIABLE(tv);

	memset(buf, 0, bufsz);
	memset(&read_overlapped, 0, sizeof(read_overlapped));

	read_overlapped.hEvent = CreateEvent(
		NULL,  /* Security */
		FALSE, /* auto-reset */
		FALSE, /* initial state = non signaled */
		NULL   /* no name */
	);

	if (read_overlapped.hEvent == NULL) {
		upslogx(LOG_ERR, "Can't create event for reading event log");
		return -1;
	}

	/* Start a read IO so we could wait on the event associated with it */
	ReadFile(sockfd, buf,
		bufsz - 1, /* -1 to be sure to have a trailing 0 */
		NULL, &read_overlapped);
	if (FALSE == GetOverlappedResult(sockfd, &read_overlapped, &bytesRead, FALSE))
		upsdebugx(6, "%s: pipe read error", __func__);

	ret = (ssize_t)bytesRead;
#endif  /* WIN32 */

	upsdebugx(ret > 0 ? 5 : 6,
		"%s: received %" PRIiMAX " bytes from driver socket: %s",
		__func__, (intmax_t)ret, (ret > 0 ? buf : "<null>"));
	return ret;
}

ssize_t upsdrvquery_write(TYPE_FD sockfd, const char *buf) {
	size_t	buflen = strlen(buf);

	upsdebugx(5, "%s: write to driver socket: %s", __func__, buf);

	if (INVALID_FD(sockfd)) {
		upslog_with_errno(LOG_ERR, "socket not initialized");
		return -1;
	}

#ifndef WIN32
	int ret = write(sockfd, buf, buflen);

	if (ret < 0 || ret != (int)buflen) {
		upslog_with_errno(LOG_ERR, "Write to socket %d failed", sockfd);
		/*close(fd);*/
		/*return ERROR_FD;*/
		return -1;
	}

	return ret;
#else
	DWORD	bytesWritten = 0;
	BOOL	result = FALSE;

	result = WriteFile(sockfd, buf, buflen, &bytesWritten, NULL);
	if (result == 0 || bytesWritten != (DWORD)buflen) {
		upslog_with_errno(LOG_ERR, "Write to handle %p failed", sockfd);
		/*CloseHandle(fd);*/
		/*return ERROR_FD;*/
		return -1;
	}

	return (ssize_t)bytesWritten;
#endif  /* WIN32 */
}

ssize_t upsdrvquery_prepare(TYPE_FD sockfd, struct timeval tv) {
	time_t	start, now;
	char buf[LARGEBUF];

	/* Avoid noise */
	if (upsdrvquery_write(sockfd, "NOBROADCAST\n") < 0)
		goto socket_error;

	/* flush incoming, if any */
	time(&start);

	while (1) {
		upsdrvquery_read_timeout(sockfd, tv, buf, sizeof(buf));
		time(&now);
		if (difftime(now, start) > tv.tv_sec)
			break;
		tv.tv_sec -= difftime(now, start);
	}

	/* Check that we can have a civilized dialog --
	 * nope, this one is for network protocol */
/*
	if (upsdrvquery_write(sockfd, "GET TRACKING\n") < 0)
		goto socket_error;
	if (upsdrvquery_read_timeout(sockfd, tv, buf, sizeof(buf)) < 1)
		goto socket_error;
	if (strcmp(buf, "ON")) {
		upslog_with_errno(LOG_ERR, "Driver does not have TRACKING support enabled");
		goto socket_error;
	}
*/

	return 1;

socket_error:
	upsdrvquery_close(sockfd);
	return -1;
}

/* UUID v4 basic implementation
 * Note: 'dest' must be at least `UUID4_LEN` long */
#define UUID4_BYTESIZE 16
static int upsdrvquery_nut_uuid_v4(char *uuid_str)
{
	size_t	i;
	uint8_t	nut_uuid[UUID4_BYTESIZE];

	if (!uuid_str)
		return 0;

	for (i = 0; i < UUID4_BYTESIZE; i++)
		nut_uuid[i] = (uint8_t)rand() + (uint8_t)rand();

	/* set variant and version */
	nut_uuid[6] = (nut_uuid[6] & 0x0F) | 0x40;
	nut_uuid[8] = (nut_uuid[8] & 0x3F) | 0x80;

	return snprintf(uuid_str, UUID4_LEN,
		"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		nut_uuid[0], nut_uuid[1], nut_uuid[2], nut_uuid[3],
		nut_uuid[4], nut_uuid[5], nut_uuid[6], nut_uuid[7],
		nut_uuid[8], nut_uuid[9], nut_uuid[10], nut_uuid[11],
		nut_uuid[12], nut_uuid[13], nut_uuid[14], nut_uuid[15]);
}

ssize_t upsdrvquery_request(
	TYPE_FD sockfd, struct timeval tv,
	const char *query,
	char *buf, const size_t bufsz
) {
	/* Assume TRACKING works; post a socket-protocol
	 * query to driver and return whatever it says */
	char	qbuf[LARGEBUF];
	size_t	qlen;
	char	tracking_id[UUID4_LEN];

	if (snprintf(qbuf, sizeof(qbuf), "%s", query) < 0)
		goto socket_error;

	qlen = strlen(qbuf);
	while (qlen > 0 && qbuf[qlen - 1] == '\n') {
		qbuf[qlen - 1] = '\0';
		qlen--;
	}

	upsdrvquery_nut_uuid_v4(tracking_id);

	if (snprintf(qbuf + qlen, sizeof(qbuf) - qlen, " TRACKING %s\n", tracking_id) < 0)
		goto socket_error;

	/* Post the query and wait for reply */
	if (upsdrvquery_write(sockfd, qbuf) < 0)
		goto socket_error;

	if (upsdrvquery_read_timeout(sockfd, tv, buf, bufsz) < 1)
		goto socket_error;

	if (!strncmp(buf, "TRACKING ", 9) && !strncmp(buf + 9, tracking_id, UUID4_LEN - 1)) {
		int	ret;
		size_t	offset = 9 + UUID4_LEN;
		if (sscanf(buf + offset, " %d", &ret) < 1) {
			upsdebugx(5, "%s: sscanf failed at offset %" PRIuSIZE " (char '%c')",
				__func__, offset, buf[offset]);
			goto socket_error;
		}
		upsdebugx(5, "%s: parsed out command status: %d", __func__, ret);
		return ret;
	} else {
		upsdebugx(5, "%s: response did not have expected format",
			__func__);
	}

socket_error:
	upsdrvquery_close(sockfd);
	return -1;
}

ssize_t upsdrvquery_oneshot(
	const char *drvname, const char *upsname,
	const char *query,
	char *buf, const size_t bufsz
) {
	struct timeval	tv;
	ssize_t ret;
	TYPE_FD sockfd = upsdrvquery_connect_drvname_upsname(drvname, upsname);

	if (INVALID_FD(sockfd))
		return -1;

	/* This depends on driver looping delay, polling frequency,
	 * being blocked on other commands, etc. Number so far is
	 * arbitrary and optimistic. Causes a long initial silence
	 * to flush incoming buffers after NOBROADCAST.
	 */
	tv.tv_sec = 5;
	tv.tv_usec = 0;

	/* Here we have a fragile simplistic parser that
	 * expects one line replies to a specific command,
	 * so want to rule out the noise.
	 */
	if (upsdrvquery_prepare(sockfd, tv) < 0)
		return -1;

	if ((ret = upsdrvquery_request(sockfd, tv, query, buf, bufsz)) < 0)
		return -1;

	upsdrvquery_close(sockfd);
	return ret;
}
