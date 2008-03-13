/*
 * I/O functions for libusb
 * Copyright (C) 2007-2008 Daniel Drake <dsd@gentoo.org>
 * Copyright (c) 2001 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <config.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "libusbi.h"

#define TRANSFER_TO_PRIV(trf) (container_of((trf), struct usbi_transfer, pub))

/* this is a list of in-flight rb_handles, sorted by timeout expiration.
 * URBs to timeout the soonest are placed at the beginning of the list, URBs
 * that will time out later are placed after, and urbs with infinite timeout
 * are always placed at the very end. */
static struct list_head flying_transfers;

/* list of poll fd's */
static struct list_head pollfds;

/* user callbacks for pollfd changes */
static libusb_pollfd_added_cb fd_added_cb = NULL;
static libusb_pollfd_removed_cb fd_removed_cb = NULL;

void usbi_io_init()
{
	list_init(&flying_transfers);
	list_init(&pollfds);
	fd_added_cb = NULL;
	fd_removed_cb = NULL;
}

static int calculate_timeout(struct usbi_transfer *transfer)
{
	int r;
	struct timespec current_time;
	unsigned int timeout = transfer->pub.timeout;

	if (!timeout)
		return 0;

	r = clock_gettime(CLOCK_MONOTONIC, &current_time);
	if (r < 0) {
		usbi_err("failed to read monotonic clock, errno=%d", errno);
		return r;
	}

	current_time.tv_sec += timeout / 1000;
	current_time.tv_nsec += (timeout % 1000) * 1000000;

	if (current_time.tv_nsec > 1000000000) {
		current_time.tv_nsec -= 1000000000;
		current_time.tv_sec++;
	}

	TIMESPEC_TO_TIMEVAL(&transfer->timeout, &current_time);
	return 0;
}

static void add_to_flying_list(struct usbi_transfer *transfer)
{
	struct usbi_transfer *cur;
	struct timeval *timeout = &transfer->timeout;

	/* if we have no other flying transfers, start the list with this one */
	if (list_empty(&flying_transfers)) {
		list_add(&transfer->list, &flying_transfers);
		return;
	}

	/* if we have infinite timeout, append to end of list */
	if (!timerisset(timeout)) {
		list_add_tail(&transfer->list, &flying_transfers);
		return;
	}

	/* otherwise, find appropriate place in list */
	list_for_each_entry(cur, &flying_transfers, list) {
		/* find first timeout that occurs after the transfer in question */
		struct timeval *cur_tv = &cur->timeout;

		if (!timerisset(cur_tv) || (cur_tv->tv_sec > timeout->tv_sec) ||
				(cur_tv->tv_sec == timeout->tv_sec &&
					cur_tv->tv_usec > timeout->tv_usec)) {
			list_add_tail(&transfer->list, &cur->list);
			return;
		}
	}

	/* otherwise we need to be inserted at the end */
	list_add_tail(&transfer->list, &flying_transfers);
}

static int submit_transfer(struct usbi_transfer *itransfer)
{
	int r = usbi_backend->submit_transfer(itransfer);
	if (r < 0)
		return r;

	add_to_flying_list(itransfer);
	return 0;
}

API_EXPORTED size_t libusb_get_transfer_alloc_size(void)
{
	return sizeof(struct usbi_transfer) + usbi_backend->transfer_priv_size;
}

void __init_transfer(struct usbi_transfer *transfer)
{
	memset(transfer, 0, sizeof(*transfer));
}

API_EXPORTED void libusb_init_transfer(struct libusb_transfer *transfer)
{
	__init_transfer(TRANSFER_TO_PRIV(transfer));
}

API_EXPORTED struct libusb_transfer *libusb_alloc_transfer(void)
{
	struct usbi_transfer *transfer =
		malloc(sizeof(*transfer) + usbi_backend->transfer_priv_size);
	if (!transfer)
		return NULL;

	__init_transfer(transfer);
	return &transfer->pub;
}

API_EXPORTED int libusb_submit_transfer(struct libusb_transfer *transfer)
{
	struct usbi_transfer *itransfer = TRANSFER_TO_PRIV(transfer);
	int r;

	itransfer->transferred = 0;
	r = calculate_timeout(itransfer);
	if (r < 0)
		return r;

	if (transfer->endpoint_type == LIBUSB_ENDPOINT_TYPE_CONTROL) {
		struct libusb_control_setup *setup =
			(struct libusb_control_setup *) transfer->buffer;
	
		usbi_dbg("RQT=%02x RQ=%02x VAL=%04x IDX=%04x length=%d",
			setup->bRequestType, setup->bRequest, setup->wValue, setup->wIndex,
			setup->wLength);

		setup->wValue = cpu_to_le16(setup->wValue);
		setup->wIndex = cpu_to_le16(setup->wIndex);
		setup->wLength = cpu_to_le16(setup->wLength);
	}

	return submit_transfer(itransfer);
}

API_EXPORTED int libusb_cancel_transfer(struct libusb_transfer *transfer)
{
	struct usbi_transfer *itransfer = TRANSFER_TO_PRIV(transfer);
	int r;

	usbi_dbg("");
	r = usbi_backend->cancel_transfer(itransfer);
	if (r < 0)
		usbi_err("cancel transfer failed error %d", r);
	return r;
}

API_EXPORTED int libusb_cancel_transfer_sync(struct libusb_transfer *transfer)
{
	struct usbi_transfer *itransfer = TRANSFER_TO_PRIV(transfer);
	int r;

	usbi_dbg("");
	r = usbi_backend->cancel_transfer(itransfer);
	if (r < 0) {
		usbi_err("cancel transfer failed error %d", r);
		return r;
	}

	itransfer->flags |= USBI_TRANSFER_SYNC_CANCELLED;
	while (itransfer->flags & USBI_TRANSFER_SYNC_CANCELLED) {
		r = libusb_poll();
		if (r < 0)
			return r;
	}

	return 0;
}

void usbi_handle_transfer_completion(struct usbi_transfer *itransfer,
	enum libusb_transfer_status status)
{
	struct libusb_transfer *transfer = &itransfer->pub;

	if (status == LIBUSB_TRANSFER_SILENT_COMPLETION)
		return;

	if (status == LIBUSB_TRANSFER_COMPLETED
			&& transfer->flags & LIBUSB_TRANSFER_SHORT_NOT_OK) {
		int rqlen = transfer->length;
		if (transfer->endpoint_type == LIBUSB_ENDPOINT_TYPE_CONTROL)
			rqlen -= LIBUSB_CONTROL_SETUP_SIZE;
		if (rqlen != itransfer->transferred) {
			usbi_dbg("interpreting short transfer as error");
			status = LIBUSB_TRANSFER_ERROR;
		}
	}

	transfer->status = status;
	transfer->actual_length = itransfer->transferred;
	if (transfer->callback)
		transfer->callback(transfer);
	if (transfer->flags & LIBUSB_TRANSFER_FREE_TRANSFER)
		libusb_free_transfer(transfer);
}

void usbi_handle_transfer_cancellation(struct usbi_transfer *transfer)
{
	/* if the URB is being cancelled synchronously, raise cancellation
	 * completion event by unsetting flag, and ensure that user callback does
	 * not get called.
	 */
	if (transfer->flags & USBI_TRANSFER_SYNC_CANCELLED) {
		transfer->flags &= ~USBI_TRANSFER_SYNC_CANCELLED;
		usbi_dbg("detected sync. cancel");
		usbi_handle_transfer_completion(transfer,
			LIBUSB_TRANSFER_SILENT_COMPLETION);
		return;
	}

	/* if the URB was cancelled due to timeout, report timeout to the user */
	if (transfer->flags & USBI_TRANSFER_TIMED_OUT) {
		usbi_dbg("detected timeout cancellation");
		usbi_handle_transfer_completion(transfer, LIBUSB_TRANSFER_TIMED_OUT);
		return;
	}

	/* otherwise its a normal async cancel */
	usbi_handle_transfer_completion(transfer, LIBUSB_TRANSFER_CANCELLED);
}

static void handle_timeout(struct usbi_transfer *itransfer)
{
	/* handling timeouts is tricky, as we may race with the kernel: we may
	 * detect a timeout racing with the condition that the urb has actually
	 * completed. we asynchronously cancel the URB and report timeout
	 * to the user when the URB cancellation completes (or not at all if the
	 * URB actually gets delivered as per this race) */
	struct libusb_transfer *transfer = &itransfer->pub;
	int r;

	itransfer->flags |= USBI_TRANSFER_TIMED_OUT;
	r = libusb_cancel_transfer(transfer);
	if (r < 0)
		usbi_warn("async cancel failed %d errno=%d", r, errno);
}

static int handle_timeouts(void)
{
	struct timespec systime_ts;
	struct timeval systime;
	struct usbi_transfer *transfer;
	int r;

	if (list_empty(&flying_transfers))
		return 0;

	/* get current time */
	r = clock_gettime(CLOCK_MONOTONIC, &systime_ts);
	if (r < 0)
		return r;

	TIMESPEC_TO_TIMEVAL(&systime, &systime_ts);

	/* iterate through flying transfers list, finding all transfers that
	 * have expired timeouts */
	list_for_each_entry(transfer, &flying_transfers, list) {
		struct timeval *cur_tv = &transfer->timeout;

		/* if we've reached transfers of infinite timeout, we're all done */
		if (!timerisset(cur_tv))
			return 0;

		/* ignore timeouts we've already handled */
		if (transfer->flags & USBI_TRANSFER_TIMED_OUT)
			continue;

		/* if transfer has non-expired timeout, nothing more to do */
		if ((cur_tv->tv_sec > systime.tv_sec) ||
				(cur_tv->tv_sec == systime.tv_sec &&
					cur_tv->tv_usec > systime.tv_usec))
			return 0;
	
		/* otherwise, we've got an expired timeout to handle */
		handle_timeout(transfer);
	}

	return 0;
}

static int poll_io(struct timeval *tv)
{
	int r;
	int maxfd = 0;
	fd_set readfds, writefds;
	fd_set *_readfds = NULL;
	fd_set *_writefds = NULL;
	struct usbi_pollfd *ipollfd;
	int have_readfds = 0;
	int have_writefds = 0;
	struct timeval select_timeout;
	struct timeval timeout;

	r = libusb_get_next_timeout(&timeout);
	if (r) {
		/* timeout already expired? */
		if (!timerisset(&timeout))
			return handle_timeouts();

		/* choose the smallest of next URB timeout or user specified timeout */
		if (timercmp(&timeout, tv, <))
			select_timeout = timeout;
		else
			select_timeout = *tv;
	} else {
		select_timeout = *tv;
	}

	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	list_for_each_entry(ipollfd, &pollfds, list) {
		struct libusb_pollfd *pollfd = &ipollfd->pollfd;
		int fd = pollfd->fd;
		if (pollfd->events & POLLIN) {
			have_readfds = 1;
			FD_SET(fd, &readfds);
		}
		if (pollfd->events & POLLOUT) {
			have_writefds = 1;
			FD_SET(fd, &writefds);
		}
		if (fd > maxfd)
			maxfd = fd;
	}

	if (have_readfds)
		_readfds = &readfds;
	if (have_writefds)
		_writefds = &writefds;

	usbi_dbg("select() with timeout in %d.%06ds", select_timeout.tv_sec,
		select_timeout.tv_usec);
	r = select(maxfd + 1, _readfds, _writefds, NULL, &select_timeout);
	usbi_dbg("select() returned %d with %d.%06ds remaining",
		r, select_timeout.tv_sec, select_timeout.tv_usec);
	if (r == 0) {
		*tv = select_timeout;
		return handle_timeouts();
	} else if (r == -1 && errno == EINTR) {
		return 0;
	} else if (r < 0) {
		usbi_err("select failed %d err=%d\n", r, errno);
		return r;
	}

	r = usbi_backend->handle_events(_readfds, _writefds);
	if (r < 0)
		return r;

	/* FIXME check return value? */
	return handle_timeouts();
}

API_EXPORTED int libusb_poll_timeout(struct timeval *tv)
{
	return poll_io(tv);
}

API_EXPORTED int libusb_poll(void)
{
	struct timeval tv;
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	return poll_io(&tv);
}

API_EXPORTED int libusb_get_next_timeout(struct timeval *tv)
{
	struct usbi_transfer *transfer;
	struct timespec cur_ts;
	struct timeval cur_tv;
	struct timeval *next_timeout;
	int r;
	int found = 0;

	if (list_empty(&flying_transfers)) {
		usbi_dbg("no URBs, no timeout!");
		return 0;
	}

	/* find next transfer which hasn't already been processed as timed out */
	list_for_each_entry(transfer, &flying_transfers, list) {
		if (!(transfer->flags & USBI_TRANSFER_TIMED_OUT)) {
			found = 1;
			break;
		}
	}

	if (!found) {
		usbi_dbg("all URBs have already been processed for timeouts");
		return 0;
	}

	next_timeout = &transfer->timeout;

	/* no timeout for next transfer */
	if (!timerisset(next_timeout)) {
		usbi_dbg("no URBs with timeouts, no timeout!");
		return 0;
	}

	r = clock_gettime(CLOCK_MONOTONIC, &cur_ts);
	if (r < 0) {
		usbi_err("failed to read monotonic clock, errno=%d", errno);
		return r;
	}
	TIMESPEC_TO_TIMEVAL(&cur_tv, &cur_ts);

	if (timercmp(&cur_tv, next_timeout, >=)) {
		usbi_dbg("first timeout already expired");
		timerclear(tv);
	} else {
		timersub(next_timeout, &cur_tv, tv);
		usbi_dbg("next timeout in %d.%06ds", tv->tv_sec, tv->tv_usec);
	}

	return 1;
}

API_EXPORTED void libusb_free_transfer(struct libusb_transfer *transfer)
{
	struct usbi_transfer *itransfer;
	if (!transfer)
		return;

	if (transfer->flags & LIBUSB_TRANSFER_FREE_BUFFER)
		free(transfer->buffer);

	itransfer = TRANSFER_TO_PRIV(transfer);
	free(itransfer);
}

API_EXPORTED void libusb_set_pollfd_notifiers(libusb_pollfd_added_cb added_cb,
	libusb_pollfd_removed_cb removed_cb)
{
	fd_added_cb = added_cb;
	fd_removed_cb = removed_cb;
}

int usbi_add_pollfd(int fd, short events)
{
	struct usbi_pollfd *ipollfd = malloc(sizeof(*ipollfd));
	if (!ipollfd)
		return -ENOMEM;

	usbi_dbg("add fd %d events %d", fd, events);
	ipollfd->pollfd.fd = fd;
	ipollfd->pollfd.events = events;
	list_add(&ipollfd->list, &pollfds);

	if (fd_added_cb)
		fd_added_cb(fd, events);
	return 0;
}

void usbi_remove_pollfd(int fd)
{
	struct usbi_pollfd *ipollfd;
	int found = 0;

	usbi_dbg("remove fd %d", fd);
	list_for_each_entry(ipollfd, &pollfds, list)
		if (ipollfd->pollfd.fd == fd) {
			found = 1;
			break;
		}

	if (!found) {
		usbi_err("couldn't find fd %d to remove", fd);
		return;
	}

	list_del(&ipollfd->list);
	free(ipollfd);
	if (fd_removed_cb)
		fd_removed_cb(fd);
}

API_EXPORTED struct libusb_pollfd **libusb_get_pollfds(void)
{
	struct libusb_pollfd **ret;
	struct usbi_pollfd *ipollfd;
	size_t i = 0;
	size_t cnt = 0;

	list_for_each_entry(ipollfd, &pollfds, list)
		cnt++;

	ret = calloc(cnt + 1, sizeof(struct libusb_pollfd *));
	if (!ret)
		return NULL;

	list_for_each_entry(ipollfd, &pollfds, list)
		ret[i++] = (struct libusb_pollfd *) ipollfd;
	ret[cnt] = NULL;

	return ret;
}

