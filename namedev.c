/*
 * namedev.c
 *
 * Userspace devfs
 *
 * Copyright (C) 2003 Greg Kroah-Hartman <greg@kroah.com>
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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>

#include "libsysfs/sysfs/libsysfs.h"
#include "list.h"
#include "udev.h"
#include "udev_lib.h"
#include "udev_version.h"
#include "logging.h"
#include "namedev.h"
#include "udev_db.h"

static struct sysfs_attribute *find_sysfs_attribute(struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device, char *attr);

LIST_HEAD(config_device_list);
LIST_HEAD(perm_device_list);


/* compare string with pattern (supports * ? [0-9] [!A-Z]) */
static int strcmp_pattern(const char *p, const char *s)
{
	if (s[0] == '\0') {
		while (p[0] == '*')
			p++;
		return (p[0] != '\0');
	}
	switch (p[0]) {
	case '[':
		{
			int not = 0;
			p++;
			if (p[0] == '!') {
				not = 1;
				p++;
			}
			while ((p[0] != '\0') && (p[0] != ']')) {
				int match = 0;
				if (p[1] == '-') {
					if ((s[0] >= p[0]) && (s[0] <= p[2]))
						match = 1;
					p += 3;
				} else {
					match = (p[0] == s[0]);
					p++;
				}
				if (match ^ not) {
					while ((p[0] != '\0') && (p[0] != ']'))
						p++;
					if (p[0] == ']')
						return strcmp_pattern(p+1, s+1);
				}
			}
		}
		break;
	case '*':
		if (strcmp_pattern(p, s+1))
			return strcmp_pattern(p+1, s);
		return 0;
	case '\0':
		if (s[0] == '\0') {
			return 0;
		}
		break;
	default:
		if ((p[0] == s[0]) || (p[0] == '?'))
			return strcmp_pattern(p+1, s+1);
		break;
	}
	return 1;
}

static struct perm_device *find_perm(char *name)
{
	struct perm_device *perm;

	list_for_each_entry(perm, &perm_device_list, node) {
		if (strcmp_pattern(perm->name, name))
			continue;
		return perm;
	}
	return NULL;
}

static mode_t get_default_mode(void)
{
	mode_t mode = 0600;	/* default to owner rw only */

	if (strlen(default_mode_str) != 0)
		mode = strtol(default_mode_str, NULL, 8);

	return mode;
}

static char *get_default_owner(void)
{
	if (strlen(default_owner_str) == 0)
		strfieldcpy(default_owner_str, "root");

	return default_owner_str;
}

static char *get_default_group(void)
{
	if (strlen(default_group_str) == 0)
		strfieldcpy(default_group_str, "root");

	return default_group_str;
}

/* extract possible {attr} and move str behind it */
static char *get_format_attribute(char **str)
{
	char *pos;
	char *attr = NULL;

	if (*str[0] == '{') {
		pos = strchr(*str, '}');
		if (pos == NULL) {
			dbg("missing closing brace for format");
			return NULL;
		}
		pos[0] = '\0';
		attr = *str+1;
		*str = pos+1;
		dbg("attribute='%s', str='%s'", attr, *str);
	}
	return attr;
}

/* extract possible format length and move str behind it*/
static int get_format_len(char **str)
{
	int num;
	char *tail;

	if (isdigit(*str[0])) {
		num = (int) strtoul(*str, &tail, 10);
		if (num > 0) {
			*str = tail;
			dbg("format length=%i", num);
			return num;
		} else {
			dbg("format parsing error '%s'", *str);
		}
	}
	return -1;
}

/** Finds the lowest positive N such that <name>N isn't present in 
 *  $(udevroot) either as a file or a symlink.
 *
 *  @param  name                Name to check for
 *  @return                     0 if <name> didn't exist and N otherwise.
 */
static int find_free_number(struct udevice *udev, const char *name)
{
	char filename[NAME_SIZE];
	int num = 0;
	struct udevice db_udev;

	strfieldcpy(filename, name);
	while (1) {
		dbg("look for existing node '%s'", filename);
		memset(&db_udev, 0x00, sizeof(struct udevice));
		if (udev_db_get_device_byname(&db_udev, filename) != 0) {
			dbg("free num=%d", num);
			return num;
		}

		num++;
		if (num > 1000) {
			info("find_free_number gone crazy (num=%d), aborted", num);
			return -1;
		}
		snprintf(filename, NAME_SIZE, "%s%d", name, num);
		filename[NAME_SIZE-1] = '\0';
	}
}

static void apply_format(struct udevice *udev, char *string, size_t maxsize,
			 struct sysfs_class_device *class_dev,
			 struct sysfs_device *sysfs_device)
{
	char temp[NAME_SIZE];
	char temp2[NAME_SIZE];
	char *tail;
	char *pos;
	char *attr;
	int len;
	int i;
	char c;
	char *spos;
	char *rest;
	int slen;
	struct sysfs_attribute *tmpattr;
	unsigned int next_free_number;

	pos = string;
	while (1) {
		pos = strchr(pos, '%');
		if (pos == NULL)
			break;

		pos[0] = '\0';
		tail = pos+1;
		len = get_format_len(&tail);
		c = tail[0];
		strfieldcpy(temp, tail+1);
		tail = temp;
		dbg("format=%c, string='%s', tail='%s'",c , string, tail);
		attr = get_format_attribute(&tail);


		switch (c) {
		case 'b':
			if (strlen(udev->bus_id) == 0)
				break;
			strfieldcatmax(string, udev->bus_id, maxsize);
			dbg("substitute bus_id '%s'", udev->bus_id);
			break;
		case 'k':
			if (strlen(udev->kernel_name) == 0)
				break;
			strfieldcatmax(string, udev->kernel_name, maxsize);
			dbg("substitute kernel name '%s'", udev->kernel_name);
			break;
		case 'n':
			if (strlen(udev->kernel_number) == 0)
				break;
			strfieldcatmax(string, udev->kernel_number, maxsize);
			dbg("substitute kernel number '%s'", udev->kernel_number);
				break;
		case 'm':
			strintcatmax(string, udev->minor, maxsize);
			dbg("substitute minor number '%u'", udev->minor);
			break;
		case 'M':
			strintcatmax(string, udev->major, maxsize);
			dbg("substitute major number '%u'", udev->major);
			break;
		case 'c':
			if (strlen(udev->program_result) == 0)
				break;
			/* get part part of the result string */
			i = 0;
			if (attr != NULL)
				i = strtoul(attr, &rest, 10);
			if (i > 0) {
				foreach_strpart(udev->program_result, " \n\r", spos, slen) {
					i--;
					if (i == 0)
						break;
				}
				if (i > 0) {
					dbg("requested part of result string not found");
					break;
				}
				if (rest[0] == '+')
					strfieldcpy(temp2, spos);
				else
					strfieldcpymax(temp2, spos, slen+1);
				strfieldcatmax(string, temp2, maxsize);
				dbg("substitute part of result string '%s'", temp2);
			} else {
				strfieldcatmax(string, udev->program_result, maxsize);
				dbg("substitute result string '%s'", udev->program_result);
			}
			break;
		case 's':
			if (attr != NULL) {
				tmpattr = find_sysfs_attribute(class_dev, sysfs_device, attr);
				if (tmpattr == NULL) {
					dbg("sysfa attribute '%s' not found", attr);
					break;
				}
				/* strip trailing whitespace of matching value */
				if (isspace(tmpattr->value[strlen(tmpattr->value)-1])) {
					i = len = strlen(tmpattr->value);
					while (i > 0 &&  isspace(tmpattr->value[i-1]))
						i--;
					if (i < len) {
						tmpattr->value[i] = '\0';
						dbg("remove %i trailing whitespace chars from '%s'",
							 len - i, tmpattr->value);
					}
				}
				strfieldcatmax(string, tmpattr->value, maxsize);
				dbg("substitute sysfs value '%s'", tmpattr->value);
			} else {
				dbg("missing attribute");
			}
			break;
		case '%':
			strfieldcatmax(string, "%", maxsize);
			pos++;
			break;
		case 'e':
			next_free_number = find_free_number(udev, string);
			if (next_free_number > 0) {
				sprintf(temp2, "%d", next_free_number);
				strfieldcatmax(string, temp2, maxsize);
			}
			break;
		default:
			dbg("unknown substitution type '%%%c'", c);
			break;
		}
		/* truncate to specified length */
		if (len > 0)
			pos[len] = '\0';

		strfieldcatmax(string, tail, maxsize);
	}
}

static void fix_kernel_name(struct udevice *udev)
{
	char *temp = udev->kernel_name;

	while (*temp != 0x00) {
		/* Some block devices have a ! in their name, 
		 * we need to change that to / */
		if (*temp == '!')
			*temp = '/';
		++temp;
	}
}

static int execute_program(struct udevice *udev, const char *path, char *value, int len)
{
	int retval;
	int count;
	int status;
	int fds[2];
	pid_t pid;
	char *pos;
	char arg[PROGRAM_SIZE];
	char *argv[(PROGRAM_SIZE / 2) + 1];
	int i;

	strfieldcpy(arg, path);
	i = 0;
	if (strchr(path, ' ')) {
		pos = arg;
		while (pos != NULL) {
			if (pos[0] == '\'') {
				/* don't separate if in apostrophes */
				pos++;
				argv[i] = strsep(&pos, "\'");
				while (pos && pos[0] == ' ')
					pos++;
			} else {
				argv[i] = strsep(&pos, " ");
			}
			dbg("arg[%i] '%s'", i, argv[i]);
			i++;
		}
		argv[i] =  NULL;
		dbg("execute '%s' with parsed arguments", arg);
	} else {
		argv[0] = arg;
		argv[1] = udev->subsystem;
		argv[2] = NULL;
		dbg("execute '%s' with subsystem '%s' argument", arg, argv[1]);
	}

	retval = pipe(fds);
	if (retval != 0) {
		dbg("pipe failed");
		return -1;
	}

	pid = fork();
	switch(pid) {
	case 0:
		/* child */
		/* dup2 write side of pipe to STDOUT */
		dup2(fds[1], STDOUT_FILENO);
		retval = execv(arg, argv);

		info(FIELD_PROGRAM " execution of '%s' failed", path);
		exit(1);
	case -1:
		dbg("fork failed");
		return -1;
	default:
		/* parent reads from fds[0] */
		close(fds[1]);
		retval = 0;
		i = 0;
		while (1) {
			count = read(fds[0], value + i, len - i-1);
			if (count <= 0)
				break;

			i += count;
			if (i >= len-1) {
				dbg("result len %d too short", len);
				retval = -1;
				break;
			}
		}

		if (count < 0) {
			dbg("read failed with '%s'", strerror(errno));
			retval = -1;
		}

		if (i > 0 && value[i-1] == '\n')
			i--;
		value[i] = '\0';
		dbg("result is '%s'", value);

		close(fds[0]);
		waitpid(pid, &status, 0);

		if (!WIFEXITED(status) || (WEXITSTATUS(status) != 0)) {
			dbg("exec program status 0x%x", status);
			retval = -1;
		}
	}
	return retval;
}

static struct sysfs_attribute *find_sysfs_attribute(struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device, char *attr)
{
	struct sysfs_attribute *tmpattr = NULL;
	char *c;

	dbg("look for device attribute '%s'", attr);
	/* try to find the attribute in the class device directory */
	tmpattr = sysfs_get_classdev_attr(class_dev, attr);
	if (tmpattr)
		goto attr_found;

	/* look in the class device directory if present */
	if (sysfs_device) {
		tmpattr = sysfs_get_device_attr(sysfs_device, attr);
		if (tmpattr)
			goto attr_found;
	}

	return NULL;

attr_found:
	c = strchr(tmpattr->value, '\n');
	if (c != NULL)
		c[0] = '\0';

	dbg("found attribute '%s'", tmpattr->path);
	return tmpattr;
}

static int compare_sysfs_attribute(struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device, struct sysfs_pair *pair)
{
	struct sysfs_attribute *tmpattr;
	int i;
	int len;

	if ((pair == NULL) || (pair->file[0] == '\0') || (pair->value == '\0'))
		return -ENODEV;

	tmpattr = find_sysfs_attribute(class_dev, sysfs_device, pair->file);
	if (tmpattr == NULL)
		return -ENODEV;

	/* strip trailing whitespace of value, if not asked to match for it */
	if (! isspace(pair->value[strlen(pair->value)-1])) {
		i = len = strlen(tmpattr->value);
		while (i > 0 &&  isspace(tmpattr->value[i-1]))
			i--;
		if (i < len) {
			tmpattr->value[i] = '\0';
			dbg("remove %i trailing whitespace chars from '%s'",
			    len - i, tmpattr->value);
		}
	}

	dbg("compare attribute '%s' value '%s' with '%s'",
		  pair->file, tmpattr->value, pair->value);
	if (strcmp_pattern(pair->value, tmpattr->value) != 0)
		return -ENODEV;

	dbg("found matching attribute '%s' with value '%s'",
	    pair->file, pair->value);
	return 0;
}

static int match_sysfs_pairs(struct config_device *dev, struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device)
{
	struct sysfs_pair *pair;
	int i;

	for (i = 0; i < MAX_SYSFS_PAIRS; ++i) {
		pair = &dev->sysfs_pair[i];
		if ((pair->file[0] == '\0') || (pair->value[0] == '\0'))
			break;
		if (compare_sysfs_attribute(class_dev, sysfs_device, pair) != 0) {
			dbg("sysfs attribute doesn't match");
			return -ENODEV;
		}
	}

	return 0;
}

static int match_id(struct config_device *dev, struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device)
{
	char path[SYSFS_PATH_MAX];
	char *temp = NULL;

	/* we have to have a sysfs device for ID to work */
	if (!sysfs_device)
		return -ENODEV;

	strfieldcpy(path, sysfs_device->path);
	temp = strrchr(path, '/');
	temp++;
	dbg("search '%s' in '%s', path='%s'", dev->id, temp, path);
	if (strcmp_pattern(dev->id, temp) != 0)
		return -ENODEV;
	else
		return 0;
}

static int match_place(struct config_device *dev, struct sysfs_class_device *class_dev, struct sysfs_device *sysfs_device)
{
	char path[SYSFS_PATH_MAX];
	int found;
	char *temp = NULL;

	/* we have to have a sysfs device for PLACE to work */
	if (!sysfs_device)
		return -ENODEV;

	found = 0;
	strfieldcpy(path, sysfs_device->path);
	temp = strrchr(path, '/');
	dbg("search '%s' in '%s', path='%s'", dev->place, temp, path);
	if (strstr(temp, dev->place) != NULL) {
		found = 1;
	} else {
		*temp = 0x00;
		temp = strrchr(path, '/');
		dbg("search '%s' in '%s', path='%s'", dev->place, temp, path);
		if (strstr(temp, dev->place) != NULL)
			found = 1;
	}
	if (!found) {
		dbg("place doesn't match");
		return -ENODEV;
	}

	return 0;
}

static int match_rule(struct config_device *dev, struct sysfs_class_device *class_dev, struct udevice *udev, struct sysfs_device *sysfs_device)
{
	while (1) {
		/* check for matching bus value */
		if (dev->bus[0] != '\0') {
			if (sysfs_device == NULL) {
				dbg("device has no bus");
				goto try_parent;
			}
			dbg("check for " FIELD_BUS " dev->bus='%s' sysfs_device->bus='%s'", dev->bus, sysfs_device->bus);
			if (strcmp_pattern(dev->bus, sysfs_device->bus) != 0) {
				dbg(FIELD_BUS " is not matching");
				goto try_parent;
			} else {
				dbg(FIELD_BUS " matches");
			}
		}

		/* check for matching kernel name */
		if (dev->kernel[0] != '\0') {
			dbg("check for " FIELD_KERNEL " dev->kernel='%s' class_dev->name='%s'", dev->kernel, class_dev->name);
			if (strcmp_pattern(dev->kernel, class_dev->name) != 0) {
				dbg(FIELD_KERNEL " is not matching");
				goto try_parent;
			} else {
				dbg(FIELD_KERNEL " matches");
			}
		}

		/* check for matching subsystem */
		if (dev->subsystem[0] != '\0') {
			dbg("check for " FIELD_SUBSYSTEM " dev->subsystem='%s' class_dev->name='%s'", dev->subsystem, class_dev->name);
			if (strcmp_pattern(dev->subsystem, udev->subsystem) != 0) {
				dbg(FIELD_SUBSYSTEM " is not matching");
				goto try_parent;
			} else {
				dbg(FIELD_SUBSYSTEM " matches");
			}
		}

		/* check for matching bus id */
		if (dev->id[0] != '\0') {
			dbg("check " FIELD_ID);
			if (match_id(dev, class_dev, sysfs_device) != 0) {
				dbg(FIELD_ID " is not matching");
				goto try_parent;
			} else {
				dbg(FIELD_ID " matches");
			}
		}

		/* check for matching place of device */
		if (dev->place[0] != '\0') {
			dbg("check " FIELD_PLACE);
			if (match_place(dev, class_dev, sysfs_device) != 0) {
				dbg(FIELD_PLACE " is not matching");
				goto try_parent;
			} else {
				dbg(FIELD_PLACE " matches");
			}
		}

		/* check for matching sysfs pairs */
		if (dev->sysfs_pair[0].file[0] != '\0') {
			dbg("check " FIELD_SYSFS " pairs");
			if (match_sysfs_pairs(dev, class_dev, sysfs_device) != 0) {
				dbg(FIELD_SYSFS " is not matching");
				goto try_parent;
			} else {
				dbg(FIELD_SYSFS " matches");
			}
		}

		/* execute external program */
		if (dev->program[0] != '\0') {
			char program[PROGRAM_SIZE];

			dbg("check " FIELD_PROGRAM);
			strfieldcpy(program, dev->program);
			apply_format(udev, program, sizeof(program), class_dev, sysfs_device);
			if (execute_program(udev, program, udev->program_result, NAME_SIZE) != 0) {
				dbg(FIELD_PROGRAM " returned nonzero");
				goto try_parent;
			} else {
				dbg(FIELD_PROGRAM " returned successful");
			}
		}

		/* check for matching result of external program */
		if (dev->result[0] != '\0') {
			dbg("check for " FIELD_RESULT
			    " dev->result='%s', udev->program_result='%s'",
			    dev->result, udev->program_result);
			if (strcmp_pattern(dev->result, udev->program_result) != 0) {
				dbg(FIELD_RESULT " is not matching");
				goto try_parent;
			} else {
				dbg(FIELD_RESULT " matches");
			}
		}

		/* Yeah, we matched! */
		return 0;

try_parent:
		dbg("try parent sysfs device");
		sysfs_device = sysfs_get_device_parent(sysfs_device);
		if (sysfs_device == NULL)
			return -ENODEV;
		dbg("sysfs_device->path='%s'", sysfs_device->path);
		dbg("sysfs_device->bus_id='%s'", sysfs_device->bus_id);
		dbg("sysfs_device->bus='%s'", sysfs_device->bus);
	}

}

int namedev_name_device(struct udevice *udev, struct sysfs_class_device *class_dev)
{
	struct sysfs_class_device *class_dev_parent;
	struct sysfs_device *sysfs_device = NULL;
	struct config_device *dev;
	struct perm_device *perm;
	struct sysinfo info;
	char *pos;

	udev->mode = 0;
	dbg("class_dev->name='%s'", class_dev->name);

	/* Figure out where the "device"-symlink is at.  For char devices this will
	 * always be in the class_dev->path.  On block devices, only the main block
	 * device will have the device symlink in it's path. All partition devices
	 * need to look at the symlink in its parent directory.
	 */
	class_dev_parent = sysfs_get_classdev_parent(class_dev);
	if (class_dev_parent != NULL) {
		dbg("given class device has a parent, use this instead");
		sysfs_device = sysfs_get_classdev_device(class_dev_parent);
	} else {
		sysfs_device = sysfs_get_classdev_device(class_dev);
	}

	if (sysfs_device) {
		dbg("found devices device: path='%s', bus_id='%s', bus='%s'",
		    sysfs_device->path, sysfs_device->bus_id, sysfs_device->bus);
		strfieldcpy(udev->bus_id, sysfs_device->bus_id);
		strfieldcpy(udev->bus, sysfs_device->bus);
	}

	strfieldcpy(udev->kernel_name, class_dev->name);
	fix_kernel_name(udev);
	dbg("udev->kernel_name = '%s'", udev->kernel_name);

	/* get kernel number */
	pos = class_dev->name + strlen(class_dev->name);
	while (isdigit(*(pos-1)))
		pos--;
	strfieldcpy(udev->kernel_number, pos);
	dbg("kernel_number='%s'", udev->kernel_number);

	/* look for a matching rule to apply */
	list_for_each_entry(dev, &config_device_list, node) {
		dbg("process rule");
		if (match_rule(dev, class_dev, udev, sysfs_device) == 0) {
			if (dev->name[0] == '\0' && dev->symlink[0] == '\0') {
				info("configured rule in '%s' at line %i applied, '%s' is ignored",
				     dev->config_file, dev->config_line, udev->kernel_name);
				return -1;
			}

			if (dev->symlink[0] != '\0') {
				char temp[NAME_SIZE];

				info("configured rule in '%s' at line %i applied, added symlink '%s'",
				     dev->config_file, dev->config_line, dev->symlink);
				strfieldcpy(temp, dev->symlink);
				apply_format(udev, temp, sizeof(temp), class_dev, sysfs_device);
				if (udev->symlink[0] != '\0')
					strfieldcat(udev->symlink, " ");
				strfieldcat(udev->symlink, temp);
			}

			if (dev->name[0] != '\0') {
				/* apply all_partitions flag only at a main block device */
				if (dev->partitions > 0 &&
				    (udev->type != 'b' || udev->kernel_number[0] != '\0'))
					continue;

				info("configured rule in '%s' at line %i applied, '%s' becomes '%s'",
				     dev->config_file, dev->config_line, udev->kernel_name, dev->name);
				strfieldcpy(udev->name, dev->name);
				goto found;
			}
		}
	}
	/* no rule was found so we use the kernel name */
	strfieldcpy(udev->name, udev->kernel_name);
	if (udev->type == 'n')
		goto done;
	else
		goto perms;

found:
	apply_format(udev, udev->name, sizeof(udev->name), class_dev, sysfs_device);
	strfieldcpy(udev->config_file, dev->config_file);
	udev->config_line = dev->config_line;

	if (udev->type == 'n')
		goto done;

	udev->partitions = dev->partitions;

	/* get permissions given in rule */
	set_empty_perms(udev, dev->mode,
			      dev->owner,
			      dev->group);

perms:
	/* get permissions given in config file or set defaults */
	perm = find_perm(udev->name);
	if (perm != NULL) {
		set_empty_perms(udev, perm->mode,
				      perm->owner,
				      perm->group);
	}
	set_empty_perms(udev, get_default_mode(),
			      get_default_owner(),
			      get_default_group());

	dbg("name, '%s' is going to have owner='%s', group='%s', mode = %#o",
	    udev->name, udev->owner, udev->group, udev->mode);

done:
	/* store time of action */
	sysinfo(&info);
	udev->config_uptime = info.uptime;

	return 0;
}

int namedev_init(void)
{
	int retval;

	retval = namedev_init_rules();
	if (retval)
		return retval;

	retval = namedev_init_permissions();
	if (retval)
		return retval;

	dump_config_dev_list();
	dump_perm_dev_list();
	return retval;
}
