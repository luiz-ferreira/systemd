/*
 * scsi_id.c
 *
 * Main section of the scsi_id program
 *
 * Copyright (C) IBM Corp. 2003
 *
 *  This library is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation; either version 2.1 of the
 *  License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 *  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sysfs/libsysfs.h>
#include "scsi_id_version.h"
#include "scsi_id.h"

#ifndef SCSI_ID_VERSION
#warning No version
#define SCSI_ID_VERSION	"unknown"
#endif

/*
 * temporary names for mknod.
 */
#define TMP_DIR	"/tmp"
#define TMP_PREFIX "scsi"

/*
 * XXX Note the 'e' (send output to stderr in all cases), and 'c' (callout)
 * options are not supported, but other code is still left in place for
 * now.
 */
static const char short_options[] = "abd:f:gip:s:uvVx";
/*
 * Just duplicate per dev options.
 */
static const char dev_short_options[] = "bgp:";

char sysfs_mnt_path[SYSFS_PATH_MAX];

static int all_good;
static int always_info;
static char *default_callout;
static int dev_specified;
static int sys_specified;
static char config_file[MAX_NAME_LEN] = SCSI_ID_CONFIG_FILE;
static int display_bus_id;
static int default_page_code;
static int use_stderr;
static int debug;
static int hotplug_mode;
static int reformat_serial;
static int export;
static char vendor_str[64];
static char model_str[64];
static char revision_str[16];
static char type_str[16];

void log_message (int level, const char *format, ...)
{
	va_list	args;

	if (!debug && level == LOG_DEBUG)
		return;

	va_start (args, format);
	if (!hotplug_mode || use_stderr) {
		vfprintf(stderr, format, args);
	} else {
		static int logging_init = 0;
		if (!logging_init) {
			openlog ("scsi_id", LOG_PID, LOG_DAEMON);
			logging_init = 1;
		}

		vsyslog(level, format, args);
	}
	va_end (args);
	return;
}

static void set_str(char *to, const char *from, size_t count)
{
	size_t i, j, len;

	/* strip trailing whitespace */
	len = strnlen(from, count);
	while (len && isspace(from[len-1]))
		len--;

	/* strip leading whitespace */
	i = 0;
	while (isspace(from[i]) && (i < len))
		i++;

	j = 0;
	while (i < len) {
		/* substitute multiple whitespace */
		if (isspace(from[i])) {
			while (isspace(from[i]))
				i++;
			to[j++] = '_';
		}
		/* skip chars */
		if (from[i] == '/') {
			i++;
			continue;
		}
		to[j++] = from[i++];
	}
	to[j] = '\0';
}

static void set_type(char *to, const char *from, size_t len)
{
	int type_num;
	char *eptr;
	char *type = "generic";

	type_num = strtoul(from, &eptr, 0);
	if (eptr != from) {
		switch (type_num) {
		case 0:
			type = "disk";
			break;
		case 1:
			type = "tape";
			break;
		case 4:
			type = "optical";
			break;
		case 5:
			type = "cd";
			break;
		case 7:
			type = "optical";
			break;
		case 0xe:
			type = "disk";
			break;
		case 0xf:
			type = "optical";
			break;
		default:
			break;
		}
	}
	strncpy(to, type, len);
	to[len-1] = '\0';
}

static int get_major_minor(struct sysfs_class_device *class_dev, int *maj,
			   int *min)
{
	struct sysfs_attribute *dev_attr;

	dev_attr = sysfs_get_classdev_attr(class_dev, "dev");
	if (!dev_attr) {
		/*
		 * XXX This happens a lot, since sg has no dev attr.
		 * And now sysfsutils does not set a meaningful errno
		 * value. Someday change this back to a LOG_WARNING.
		 * And if sysfsutils changes, check for ENOENT and handle
		 * it separately.
		 */
		log_message(LOG_DEBUG, "%s: could not get dev attribute: %s\n",
			class_dev->name, strerror(errno));
		return -1;
	}

	dprintf("dev value %s", dev_attr->value); /* value has a trailing \n */
	if (sscanf(dev_attr->value, "%u:%u", maj, min) != 2) {
		log_message(LOG_WARNING, "%s: invalid dev major/minor\n",
			    class_dev->name);
		return -1;
	}

	return 0;
}

static int create_tmp_dev(struct sysfs_class_device *class_dev, char *tmpdev,
			  int dev_type)
{
	int maj, min;

	dprintf("(%s)\n", class_dev->name);

	if (get_major_minor(class_dev, &maj, &min))
		return -1;
	snprintf(tmpdev, MAX_NAME_LEN, "%s/%s-maj%d-min%d-%u",
		 TMP_DIR, TMP_PREFIX, maj, min, getpid());

	dprintf("tmpdev '%s'\n", tmpdev);

	if (mknod(tmpdev, 0600 | dev_type, makedev(maj, min))) {
		log_message(LOG_WARNING, "mknod failed: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static int has_sysfs_prefix(const char *path, const char *prefix)
{
	char match[MAX_NAME_LEN];

	strncpy(match, sysfs_mnt_path, MAX_NAME_LEN);
	strncat(match, prefix, MAX_NAME_LEN);
	if (strncmp(path, match, strlen(match)) == 0)
		return 1;
	else
		return 0;
}

/*
 * get_value:
 *
 * buf points to an '=' followed by a quoted string ("foo") or a string ending
 * with a space or ','.
 *
 * Return a pointer to the NUL terminated string, returns NULL if no
 * matches.
 */
static char *get_value(char **buffer)
{
	static char *quote_string = "\"\n";
	static char *comma_string = ",\n";
	char *val;
	char *end;

	if (**buffer == '"') {
		/*
		 * skip leading quote, terminate when quote seen
		 */
		(*buffer)++;
		end = quote_string;
	} else {
		end = comma_string;
	}
	val = strsep(buffer, end);
	if (val && end == quote_string)
		/*
		 * skip trailing quote
		 */
		(*buffer)++;

	while (isspace(**buffer))
		(*buffer)++;

	return val;
}

static int argc_count(char *opts)
{
	int i = 0;
	while (*opts != '\0')
		if (*opts++ == ' ')
			i++;
	return i;
}

/*
 * get_file_options:
 *
 * If vendor == NULL, find a line in the config file with only "OPTIONS=";
 * if vendor and model are set find the first OPTIONS line in the config
 * file that matches. Set argc and argv to match the OPTIONS string.
 *
 * vendor and model can end in '\n'.
 */
static int get_file_options(char *vendor, char *model, int *argc,
			    char ***newargv)
{
	char *buffer;
	FILE *fd;
	char *buf;
	char *str1;
	char *vendor_in, *model_in, *options_in; /* read in from file */
	int lineno;
	int c;
	int retval = 0;

	dprintf("vendor='%s'; model='%s'\n", vendor, model);
	fd = fopen(config_file, "r");
	if (fd == NULL) {
		dprintf("can't open %s\n", config_file);
		if (errno == ENOENT) {
			return 1;
		} else {
			log_message(LOG_WARNING, "can't open %s: %s\n",
				config_file, strerror(errno));
			return -1;
		}
	}

	/*
	 * Allocate a buffer rather than put it on the stack so we can
	 * keep it around to parse any options (any allocated newargv
	 * points into this buffer for its strings).
	 */
	buffer = malloc(MAX_BUFFER_LEN);
	if (!buffer) {
		log_message(LOG_WARNING, "Can't allocate memory.\n");
		return -1;
	}

	*newargv = NULL;
	lineno = 0;
	while (1) {
		vendor_in = model_in = options_in = NULL;

		buf = fgets(buffer, MAX_BUFFER_LEN, fd);
		if (buf == NULL)
			break;
		lineno++;
		if (buf[strlen(buffer) - 1] != '\n') {
			log_message(LOG_WARNING,
				    "Config file line %d too long.\n", lineno);
			break;
		}

		while (isspace(*buf))
			buf++;

		if (*buf == '\0')
			/*
			 * blank or all whitespace line
			 */
			continue;

		if (*buf == '#')
			/*
			 * comment line
			 */
			continue;

#ifdef LOTS
		dprintf("lineno %d: '%s'\n", lineno, buf);
#endif
		str1 = strsep(&buf, "=");
		if (str1 && strcasecmp(str1, "VENDOR") == 0) {
			str1 = get_value(&buf);
			if (!str1) {
				retval = -1;
				break;
			}
			vendor_in = str1;

			str1 = strsep(&buf, "=");
			if (str1 && strcasecmp(str1, "MODEL") == 0) {
				str1 = get_value(&buf);
				if (!str1) {
					retval = -1;
					break;
				}
				model_in = str1;
				str1 = strsep(&buf, "=");
			}
		}

		if (str1 && strcasecmp(str1, "OPTIONS") == 0) {
			str1 = get_value(&buf);
			if (!str1) {
				retval = -1;
				break;
			}
			options_in = str1;
		}
		dprintf("config file line %d:"
			" vendor '%s'; model '%s'; options '%s'\n",
			lineno, vendor_in, model_in, options_in);
		/*
		 * Only allow: [vendor=foo[,model=bar]]options=stuff
		 */
		if (!options_in || (!vendor_in && model_in)) {
			log_message(LOG_WARNING,
				    "Error parsing config file line %d '%s'\n",
				    lineno, buffer);
			retval = -1;
			break;
		}
		if (vendor == NULL) {
			if (vendor_in == NULL) {
				dprintf("matched global option\n");
				break;
			}
		} else if ((vendor_in && strncmp(vendor, vendor_in,
						 strlen(vendor_in)) == 0) &&
			   (!model_in || (strncmp(model, model_in,
						  strlen(model_in)) == 0))) {
				/*
				 * Matched vendor and optionally model.
				 *
				 * Note: a short vendor_in or model_in can
				 * give a partial match (that is FOO
				 * matches FOOBAR).
				 */
				dprintf("matched vendor/model\n");
				break;
		} else {
			dprintf("no match\n");
		}
	}

	if (retval == 0) {
		if (vendor_in != NULL || model_in != NULL ||
		    options_in != NULL) {
			/*
			 * Something matched. Allocate newargv, and store
			 * values found in options_in.
			 */
			strcpy(buffer, options_in);
			c = argc_count(buffer) + 2;
			*newargv = calloc(c, sizeof(**newargv));
			if (!*newargv) {
				log_message(LOG_WARNING,
					    "Can't allocate memory.\n");
				retval = -1;
			} else {
				*argc = c;
				c = 0;
				/*
				 * argv[0] at 0 is skipped by getopt, but
				 * store the buffer address there for
				 * alter freeing.
				 */
				(*newargv)[c] = buffer;
				for (c = 1; c < *argc; c++)
					(*newargv)[c] = strsep(&buffer, " ");
			}
		} else {
			/*
			 * No matches.
			 */
			retval = 1;
		}
	}
	if (retval != 0)
		free(buffer);
	fclose(fd);
	return retval;
}

static int set_options(int argc, char **argv, const char *short_opts,
		       char *target, char *maj_min_dev)
{
	int option;

	/*
	 * optind is a global extern used by getopt. Since we can call
	 * set_options twice (once for command line, and once for config
	 * file) we have to reset this back to 1. [Note glibc handles
	 * setting this to 0, but klibc does not.]
	 */
	optind = 1;
	while (1) {
		option = getopt(argc, argv, short_opts);
		if (option == -1)
			break;

		if (optarg)
			dprintf("option '%c' arg '%s'\n", option, optarg);
		else
			dprintf("option '%c'\n", option);

		switch (option) {
		case 'a':
			always_info = 1;
			break;
		case 'b':
			all_good = 0;
			break;

		case 'c':
			default_callout = optarg;
			break;

		case 'd':
			dev_specified = 1;
			strncpy(maj_min_dev, optarg, MAX_NAME_LEN);
			break;

		case 'e':
			use_stderr = 1;
			break;

		case 'f':
			strncpy(config_file, optarg, MAX_NAME_LEN);
			break;

		case 'g':
			all_good = 1;
			break;

		case 'i':
			display_bus_id = 1;
			break;

		case 'p':
			if (strcmp(optarg, "0x80") == 0) {
				default_page_code = 0x80;
			} else if (strcmp(optarg, "0x83") == 0) {
				default_page_code = 0x83;
			} else {
				log_message(LOG_WARNING,
					    "Unknown page code '%s'\n", optarg);
				return -1;
			}
			break;

		case 's':
			sys_specified = 1;
			strncpy(target, sysfs_mnt_path, MAX_NAME_LEN);
			strncat(target, optarg, MAX_NAME_LEN);
			break;

		case 'u':
			reformat_serial = 1;
			break;

		case 'x':
			export = 1;
			break;

		case 'v':
			debug++;
			break;

		case 'V':
			log_message(LOG_WARNING, "scsi_id version: %s\n",
				    SCSI_ID_VERSION);
			exit(0);
			break;

		default:
			log_message(LOG_WARNING,
				    "Unknown or bad option '%c' (0x%x)\n",
				    option, option);
			return -1;
		}
	}
	return 0;
}

static int per_dev_options(struct sysfs_device *scsi_dev, int *good_bad,
			   int *page_code, char *callout)
{
	int retval;
	int newargc;
	char **newargv = NULL;
	struct sysfs_attribute *vendor, *model, *type;
	int option;

	*good_bad = all_good;
	*page_code = default_page_code;
	if (default_callout && (callout != default_callout))
		strncpy(callout, default_callout, MAX_NAME_LEN);
	else
		callout[0] = '\0';

	vendor = sysfs_get_device_attr(scsi_dev, "vendor");
	if (!vendor) {
		log_message(LOG_WARNING, "%s: cannot get vendor attribute\n",
			    scsi_dev->name);
		return -1;
	}
	set_str(vendor_str, vendor->value, sizeof(vendor_str)-1);

	model = sysfs_get_device_attr(scsi_dev, "model");
	if (!model) {
		log_message(LOG_WARNING, "%s: cannot get model attribute\n",
			    scsi_dev->name);
		return -1;
	}
	set_str(model_str, model->value, sizeof(model_str)-1);

	type = sysfs_get_device_attr(scsi_dev, "type");
	if (!type) {
		log_message(LOG_WARNING, "%s: cannot get type attribute\n",
			    scsi_dev->name);
		return -1;
	}
	set_type(type_str, type->value, sizeof(type_str));

	type = sysfs_get_device_attr(scsi_dev, "rev");
	if (!type) {
		log_message(LOG_WARNING, "%s: cannot get type attribute\n",
			    scsi_dev->name);
		return -1;
	}
	set_str(revision_str, type->value, sizeof(revision_str)-1);

	retval = get_file_options(vendor->value, model->value, &newargc,
				  &newargv);

	optind = 1; /* reset this global extern */
	while (retval == 0) {
		option = getopt(newargc, newargv, dev_short_options);
		if (option == -1)
			break;

		if (optarg)
			dprintf("option '%c' arg '%s'\n", option, optarg);
		else
			dprintf("option '%c'\n", option);

		switch (option) {
		case 'b':
			*good_bad = 0;
			break;

		case 'c':
			strncpy(callout, default_callout, MAX_NAME_LEN);
			break;

		case 'g':
			*good_bad = 1;
			break;

		case 'p':
			if (strcmp(optarg, "0x80") == 0) {
				*page_code = 0x80;
			} else if (strcmp(optarg, "0x83") == 0) {
				*page_code = 0x83;
			} else {
				log_message(LOG_WARNING,
					    "Unknown page code '%s'\n", optarg);
				retval = -1;
			}
			break;

		default:
			log_message(LOG_WARNING,
				    "Unknown or bad option '%c' (0x%x)\n",
				    option, option);
			retval = -1;
			break;
		}
	}

	if (newargv) {
		free(newargv[0]);
		free(newargv);
	}
	return retval;
}

/*
 * format_serial: replace to whitespaces by underscores for calling
 * programs that use the serial for device naming (multipath, Suse
 * naming, etc...)
 */
static void format_serial(char *serial)
{
	char *p = serial;

	while (*p != '\0') {
		if (isspace(*p))
			*p = '_';
		p++;
	}
}

/*
 * scsi_id: try to get an id, if one is found, printf it to stdout.
 * returns a value passed to exit() - 0 if printed an id, else 1. This
 * could be expanded, for example, if we want to report a failure like no
 * memory etc. return 2, and return 1 for expected cases (like broken
 * device found) that do not print an id.
 */
static int scsi_id(const char *target_path, char *maj_min_dev)
{
	int retval;
	int dev_type = 0;
	char *serial, *unaligned_buf;
	struct sysfs_class_device *class_dev; /* of target_path */
	struct sysfs_class_device *class_dev_parent; /* for partitions */
	struct sysfs_device *scsi_dev; /* the scsi_device */
	int good_dev;
	int page_code;
	char callout[MAX_NAME_LEN];

	dprintf("target_path %s\n", target_path);

	/*
	 * Ugly: depend on the sysfs path to tell us whether this is a
	 * block or char device. This should probably be encoded in the
	 * "dev" along with the major/minor.
	 */
	if (has_sysfs_prefix(target_path, "/block")) {
		dev_type = S_IFBLK;
	} else if (has_sysfs_prefix(target_path, "/class")) {
		dev_type = S_IFCHR;
	} else {
		if (!hotplug_mode) {
			log_message(LOG_WARNING,
				    "Non block or class device '%s'\n",
				    target_path);
			return 1;
		} else {
			/*
			 * Expected in some cases.
			 */
			dprintf("Non block or class device\n");
			return 0;
		}
	}

	class_dev = sysfs_open_class_device_path(target_path);
	if (!class_dev) {
		log_message(LOG_WARNING, "open class %s failed: %s\n",
			    target_path, strerror(errno));
		return 1;
	}
	class_dev_parent = sysfs_get_classdev_parent(class_dev);
	dprintf("class_dev 0x%p; class_dev_parent 0x%p\n", class_dev,
		class_dev_parent);
	if (class_dev_parent) {
		scsi_dev = sysfs_get_classdev_device(class_dev_parent);
	} else {
		scsi_dev = sysfs_get_classdev_device(class_dev);
	}

	/*
	 * The close of scsi_dev will close class_dev or class_dev_parent.
	 */

	/*
	 * We assume we are called after the device is completely ready,
	 * so we don't have to loop here like udev. (And we are usually
	 * called via udev.)
	 */
	if (!scsi_dev) {
		/*
		 * errno is not set if we can't find the device link, so
		 * don't print it out here.
		 */
		log_message(LOG_WARNING, "Cannot find sysfs device associated with %s\n",
			    target_path);
		return 1;
	}


	/*
	 * Allow only scsi devices.
	 *
	 * Other block devices can support SG IO, but only ide-cd does, so
	 * for now, don't bother with anything else.
	 */
	if (strcmp(scsi_dev->bus, "scsi") != 0) {
		if (hotplug_mode)
			/*
			 * Expected in some cases.
			 */
			dprintf("%s is not a scsi device\n", target_path);
		else
			log_message(LOG_WARNING, "%s is not a scsi device\n",
				    target_path);
		return 1;
	}

	/*
	 * mknod a temp dev to communicate with the device.
	 */
	if (!dev_specified && create_tmp_dev(class_dev, maj_min_dev,
					     dev_type)) {
		dprintf("create_tmp_dev failed\n");
		return 1;
	}

	/*
	 * Get any per device (vendor + model) options from the config
	 * file.
	 */
	retval = per_dev_options(scsi_dev, &good_dev, &page_code, callout);
	dprintf("per dev options: good %d; page code 0x%x; callout '%s'\n",
		good_dev, page_code, callout);

#define ALIGN   512
	unaligned_buf = malloc(MAX_SERIAL_LEN + ALIGN);
	serial = (char*) (((unsigned long) unaligned_buf + (ALIGN - 1))
			  & ~(ALIGN - 1));
	dprintf("buffer unaligned 0x%p; aligned 0x%p\n", unaligned_buf, serial);
#undef ALIGN

	if (!good_dev) {
		retval = 1;
	} else if (callout[0] != '\0') {
		/*
		 * XXX Disabled for now ('c' is not in any options[]).
		 */
		retval = 1;
	} else if (scsi_get_serial(scsi_dev, maj_min_dev, page_code,
				   serial, MAX_SERIAL_LEN)) {
		retval = always_info?0:1;
	} else {
		retval = 0;
	}
	if (!retval) {
		if (export) {
			static char serial_str[64];
			printf("ID_VENDOR=%s\n", vendor_str);
			printf("ID_MODEL=%s\n", model_str);
			printf("ID_REVISION=%s\n", revision_str);
			set_str(serial_str, serial, sizeof(serial_str));
			printf("ID_SERIAL=%s\n", serial_str);
			printf("ID_TYPE=%s\n", type_str);
			printf("ID_BUS=scsi\n");
		} else {
			if (reformat_serial)
				format_serial(serial);
			if (display_bus_id)
				printf("%s: ", scsi_dev->name);
			printf("%s\n", serial);
		}
		dprintf("%s\n", serial);
		retval = 0;
	}
	sysfs_close_device(scsi_dev);

	if (!dev_specified)
		unlink(maj_min_dev);

	return retval;
}

int main(int argc, char **argv)
{
	int retval;
	char *devpath;
	char target_path[MAX_NAME_LEN];
	char maj_min_dev[MAX_NAME_LEN];
	int newargc;
	char **newargv;

	if (getenv("DEBUG"))
		debug++;

	dprintf("argc is %d\n", argc);
	if (sysfs_get_mnt_path(sysfs_mnt_path, MAX_NAME_LEN)) {
		log_message(LOG_WARNING, "sysfs_get_mnt_path failed: %s\n",
			strerror(errno));
		exit(1);
	}

	devpath = getenv("DEVPATH");
	if (devpath) {
		/*
		 * This implies that we were invoked via udev or hotplug.
		 */
		hotplug_mode = 1;
		sys_specified = 1;
		strncpy(target_path, sysfs_mnt_path, MAX_NAME_LEN);
		strncat(target_path, devpath, MAX_NAME_LEN);
	}

	/*
	 * Get config file options.
	 */
	newargv = NULL;
	retval = get_file_options(NULL, NULL, &newargc, &newargv);
	if (retval < 0) {
		exit(1);
	} else if (newargv && (retval == 0)) {
		if (set_options(newargc, newargv, short_options, target_path,
				maj_min_dev) < 0)
			exit(1);
		free(newargv);
	}
	/*
	 * Get command line options (overriding any config file or DEVPATH
	 * settings).
	 */
	if (set_options(argc, argv, short_options, target_path,
			maj_min_dev) < 0)
		exit(1);

	if (!sys_specified) {
		log_message(LOG_WARNING, "-s must be specified\n");
		exit(1);
	}

	retval = scsi_id(target_path, maj_min_dev);
	exit(retval);
}
