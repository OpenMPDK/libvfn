// SPDX-License-Identifier: LGPL-2.1-or-later or MIT

/*
 * This file is part of libvfn.
 *
 * Copyright (C) 2022 The libvfn Authors. All Rights Reserved.
 *
 * This library (libvfn) is dual licensed under the GNU Lesser General
 * Public License version 2.1 or later or the MIT license. See the
 * COPYING and LICENSE files for more information.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __APPLE__
#include <linux/limits.h>
#include <sys/stat.h>
#endif

#include <vfn/support.h>
#include <vfn/pci/util.h>

#include <string.h>


#ifdef __APPLE__
int pci_device_info_get_ull(const char *bdf, const char *prop, unsigned long long *v)
{
	if (!strcmp(prop, "class")) {
		// TODO: Actually read this from device once it makes sense
		*v = 0x010800;
	} else {
		return -1;
	}
	return 0;
}
#else
int pci_unbind(const char *bdf)
{
	char *path = NULL;
	struct stat sb;
	ssize_t ret;

	if (asprintf(&path, "/sys/bus/pci/devices/%s/driver/unbind", bdf) < 0) {
		log_debug("asprintf failed\n");
		return -1;
	}

	ret = stat(path, &sb);
	if (ret < 0)
		goto out;

	ret = writeall(path, bdf, strlen(bdf));

out:
	free(path);

	return ret < 0 ? -1 : 0;
}

int pci_bind(const char *bdf, const char *driver)
{
	char *path = NULL;
	ssize_t ret;

	if (asprintf(&path, "/sys/bus/pci/drivers/%s/bind", driver) < 0) {
		log_debug("asprintf failed\n");
		return -1;
	}

	ret = writeall(path, bdf, strlen(bdf));

	free(path);

	return ret < 0 ? -1 : 0;
}

int pci_driver_new_id(const char *driver, uint16_t vid, uint16_t did)
{
	char *path = NULL;
	char *id = NULL;
	ssize_t ret;

	if (asprintf(&path, "/sys/bus/pci/drivers/%s/new_id", driver) < 0) {
		log_debug("asprintf failed\n");
		return -1;
	}

	if (asprintf(&id, "%x %x", vid, did) < 0) {
		log_debug("asprintf failed\n");
		free(path);
		return -1;
	}

	ret = writeall(path, id, strlen(id));

	free(id);
	free(path);

	return ret < 0 ? -1 : 0;
}

int pci_driver_remove_id(const char *driver, uint16_t vid, uint16_t did)
{
	char *path = NULL;
	char *id = NULL;
	ssize_t ret;

	if (asprintf(&path, "/sys/bus/pci/drivers/%s/remove_id", driver) < 0) {
		log_debug("asprintf failed\n");
		return -1;
	}

	if (asprintf(&id, "%x %x", vid, did) < 0) {
		log_debug("asprintf failed\n");
		free(path);
		return -1;
	}

	ret = writeall(path, id, strlen(id));

	free(id);
	free(path);

	return ret < 0 ? -1 : 0;
}

int pci_device_info_get_ull(const char *bdf, const char *prop, unsigned long long *v)
{
	char buf[32], *endptr, *path = NULL;
	ssize_t ret;

	if (asprintf(&path, "/sys/bus/pci/devices/%s/%s", bdf, prop) < 0) {
		log_debug("asprintf failed\n");
		return -1;
	}

	ret = readmax(path, buf, 32);
	if (ret < 0)
		goto out;

	buf[ret] = '\0';

	errno = 0;
	*v = strtoull(buf, &endptr, 0);
	if (endptr == buf)
		errno = EINVAL;

out:
	free(path);

	return errno ? -1 : 0;
}

char *pci_get_driver(const char *bdf)
{
	char *p, *link = NULL, *driver = NULL, *name = NULL;
	ssize_t ret;

	if (asprintf(&link, "/sys/bus/pci/devices/%s/driver", bdf) < 0) {
		link = NULL;
		log_debug("asprintf failed\n");
		goto out;
	}

	driver = new_t(char, PATH_MAX);

	ret = readlink(link, driver, PATH_MAX - 1);
	if (ret < 0) {
		if (errno == ENOENT)
			goto out;

		log_debug("failed to resolve driver link\n");
		goto out;
	}

	driver[ret] = '\0';

	p = strrchr(driver, '/');
	if (!p) {
		log_debug("failed to determine driver name\n");
		goto out;
	}

	if (asprintf(&name, "%s", p + 1) < 0) {
		name = NULL;
		log_debug("asprintf failed\n");
		goto out;
	}

out:
	free(link);
	free(driver);

	return name;
}

char *pci_get_iommu_group(const char *bdf)
{
	char *p, *link = NULL, *group = NULL, *path = NULL;
	ssize_t ret;

	if (asprintf(&link, "/sys/bus/pci/devices/%s/iommu_group", bdf) < 0) {
		log_debug("asprintf failed\n");
		goto out;
	}

	group = new_t(char, PATH_MAX);

	ret = readlink(link, group, PATH_MAX - 1);
	if (ret < 0) {
		log_debug("failed to resolve iommu group link\n");
		goto out;
	}

	group[ret] = '\0';

	p = strrchr(group, '/');
	if (!p) {
		log_debug("failed to find iommu group number\n");
		goto out;
	}

	if (asprintf(&path, "/dev/vfio/%s", p + 1) < 0) {
		path = NULL;
		goto out;
	}

out:
	free(link);
	free(group);

	return path;
}

char *pci_get_device_vfio_id(const char *bdf)
{
	__autofree char *path = NULL;
	char *vfio_id = NULL;
	struct dirent *dentry;
	DIR *dp;

	if (asprintf(&path, "/sys/bus/pci/devices/%s/vfio-dev", bdf) < 0) {
		log_debug("asprintf failed\n");
		return NULL;
	}

	dp = opendir(path);
	if (!dp) {
		log_debug("could not open directory; is %s bound to vfio-pci?\n", bdf);
		return NULL;
	}

	do {
		/*
		 * If readdir() reaches the end of the directory stream, errno
		 * is NOT changed. errno may have been left at some non-zero
		 * value, so reset it.
		 */
		errno = 0;

		dentry = readdir(dp);
		if (!dentry) {
			if (!errno)
				errno = EINVAL;

			goto out;
		}

		if (strncmp("vfio", dentry->d_name, 4) == 0)
			break;
	} while (dentry != NULL);

	if (dentry == NULL) {
		errno = EINVAL;
		goto out;
	}

	vfio_id = strdup(dentry->d_name);
out:
	log_fatal_if(closedir(dp), "closedir");

	return vfio_id;
}
#endif

#ifdef __cplusplus
}
#endif
