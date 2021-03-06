/* SPDX-License-Identifier: LGPL-2.1-or-later or MIT */

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#ifndef LIBVFN_SUPPORT_IO_H
#define LIBVFN_SUPPORT_IO_H

/**
 * writeallfd - Write exactly count bytes to file descriptor
 * @fd: file descriptor
 * @buf: buffer to write
 * @count: number of bytes to write
 *
 * Write exactly @count bytes or error out if not possible.
 *
 * Return: @count on success, otherwise, -1 and set ``errno``.
 */
ssize_t writeallfd(int fd, const void *buf, size_t count);

/**
 * writeall - Write exactly count bytes to file
 * @path: name of file to write to
 * @buf: buffer to write
 * @count: number of bytes to write
 *
 * Write exactly @count bytes or error out if not possible.
 *
 * Return: @count on success, otherwise -1 and set ``errno``.
 */
ssize_t writeall(const char *path, const void *buf, size_t count);

/**
 * readmaxfd - Read up to count number of bytes
 * @fd: file descriptor
 * @buf: buffer to read into
 * @count: number of bytes to read
 *
 * Read up to @count number of bytes.
 *
 * Return: number of bytes read or -1 and set ``errno``.
 */
ssize_t readmaxfd(int fd, void *buf, size_t count);

/**
 * readmax - Read up to count number of bytes
 * @path: name of file to read from
 * @buf: buffer to read into
 * @count: number of bytes to read
 *
 * Read up to @count number of bytes.
 *
 * Return: number of bytes read or -1 and set ``errno``.
 */
ssize_t readmax(const char *path, void *buf, size_t count);

#endif /* LIBVFN_SUPPORT_IO_H */
