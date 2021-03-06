/***
  This file is part of systemd.

  Copyright 2016 Endless Mobile, Inc
  Copyright 2016 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include "blkid-util.h"
#include "blockdev-util.h"
#include "btrfs-util.h"
#include "dirent-util.h"
#include "efivars.h"
#include "fd-util.h"
#include "fileio.h"
#include "parse-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "stat-util.h"
#include "string-util.h"
#include "udev-util.h"
#include "unit-name.h"
#include "virt.h"

static const char *arg_dest = "/tmp";
static bool arg_enabled = true;

static int add_swap(const char *path) {
        _cleanup_free_ char *name = NULL, *unit = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(path);

        log_debug("Adding swap: %s", path);

        r = unit_name_from_path(path, ".swap", &name);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        /* FIXME: man:systemd-swap-auto-generator(8) does not exist */
        fprintf(f,
                "# Automatically generated by systemd-swap-auto-generator\n\n"
                "[Unit]\n"
                "Description=Swap Partition\n"
                "Documentation=man:systemd-swap-auto-generator(8)\n\n"
                "[Swap]\n"
                "What=%s\n",
                path);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        lnk = strjoin(arg_dest, "/" SPECIAL_SWAP_TARGET ".wants/", name, NULL);
        if (!lnk)
                return log_oom();

        mkdir_parents_label(lnk, 0755);
        if (symlink(unit, lnk) < 0)
                return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);

        return 0;
}

static int enumerate_partitions(dev_t devnum) {
        _cleanup_udev_enumerate_unref_ struct udev_enumerate *e = NULL;
        _cleanup_udev_device_unref_ struct udev_device *d = NULL;
        _cleanup_blkid_free_probe_ blkid_probe b = NULL;
        _cleanup_udev_unref_ struct udev *udev = NULL;
        struct udev_list_entry *first, *item;
        struct udev_device *parent = NULL;
        const char *name, *node, *pttype, *devtype;
        blkid_partlist pl;
        int r;
        dev_t pn;

        udev = udev_new();
        if (!udev)
                return log_oom();

        d = udev_device_new_from_devnum(udev, 'b', devnum);
        if (!d)
                return log_oom();

        name = udev_device_get_devnode(d);
        if (!name)
                name = udev_device_get_syspath(d);
        if (!name) {
                log_debug("Device %u:%u does not have a name, ignoring.",
                          major(devnum), minor(devnum));
                return 0;
        }

        parent = udev_device_get_parent(d);
        if (!parent) {
                log_debug("%s: not a partitioned device, ignoring.", name);
                return 0;
        }

        /* Does it have a devtype? */
        devtype = udev_device_get_devtype(parent);
        if (!devtype) {
                log_debug("%s: parent doesn't have a device type, ignoring.", name);
                return 0;
        }

        /* Is this a disk or a partition? We only care for disks... */
        if (!streq(devtype, "disk")) {
                log_debug("%s: parent isn't a raw disk, ignoring.", name);
                return 0;
        }

        /* Does it have a device node? */
        node = udev_device_get_devnode(parent);
        if (!node) {
                log_debug("%s: parent device does not have device node, ignoring.", name);
                return 0;
        }

        log_debug("%s: root device %s.", name, node);

        pn = udev_device_get_devnum(parent);
        if (major(pn) == 0)
                return 0;

        errno = 0;
        b = blkid_new_probe_from_filename(node);
        if (!b) {
                if (errno == 0)
                        return log_oom();

                return log_error_errno(errno, "%s: failed to allocate prober: %m", node);
        }

        blkid_probe_enable_partitions(b, 1);
        blkid_probe_set_partitions_flags(b, BLKID_PARTS_ENTRY_DETAILS);

        errno = 0;
        r = blkid_do_safeprobe(b);
        if (r == 1)
                return 0; /* no results */
        else if (r == -2) {
                log_warning("%s: probe gave ambiguous results, ignoring.", node);
                return 0;
        } else if (r != 0)
                return log_error_errno(errno ?: EIO, "%s: failed to probe: %m", node);

        errno = 0;
        r = blkid_probe_lookup_value(b, "PTTYPE", &pttype, NULL);
        if (r != 0) {
                if (errno == 0)
                        return 0; /* No partition table found. */

                return log_error_errno(errno, "%s: failed to determine partition table type: %m", node);
        }

        if (!streq_ptr(pttype, "dos")) {
                log_debug("%s: not a MSDOS partition table, ignoring.", node);
                return 0;
        }

        errno = 0;
        pl = blkid_probe_get_partitions(b);
        if (!pl) {
                if (errno == 0)
                        return log_oom();

                return log_error_errno(errno, "%s: failed to list partitions: %m", node);
        }

        e = udev_enumerate_new(udev);
        if (!e)
                return log_oom();

        r = udev_enumerate_add_match_parent(e, parent);
        if (r < 0)
                return log_oom();

        r = udev_enumerate_add_match_subsystem(e, "block");
        if (r < 0)
                return log_oom();

        r = udev_enumerate_scan_devices(e);
        if (r < 0)
                return log_error_errno(r, "%s: failed to enumerate partitions: %m", node);

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                _cleanup_udev_device_unref_ struct udev_device *q;
                const char *subnode;
                blkid_partition pp;
                dev_t qn;
                int nr;

                q = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
                if (!q)
                        continue;

                qn = udev_device_get_devnum(q);
                if (major(qn) == 0)
                        continue;

                if (qn == devnum)
                        continue;

                if (qn == pn)
                        continue;

                subnode = udev_device_get_devnode(q);
                if (!subnode)
                        continue;

                pp = blkid_partlist_devno_to_partition(pl, qn);
                if (!pp)
                        continue;

                nr = blkid_partition_get_partno(pp);
                if (nr < 0)
                        continue;

                if (blkid_partition_get_type(pp) == 0x82) { /* MBR swap */
                        int k;

                        k = add_swap(subnode);
                        if (k < 0)
                                r = k;
                }
        }

        return r;
}

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        assert(key);

        if (STR_IN_SET(key, "systemd.swap_auto", "rd.systemd.swap_auto") && value) {

                r = parse_boolean(value);
                if (r < 0)
                        log_warning("Failed to parse swap-auto switch \"%s\". Ignoring.", value);
                else
                        arg_enabled = r;

        }

        return 0;
}

static int add_mounts(void) {
        dev_t devno;
        int r;

        r = get_block_device_harder("/", &devno);
        if (r < 0)
                return log_error_errno(r, "Failed to determine block device of root file system: %m");
        else if (r == 0) {
                r = get_block_device_harder("/usr", &devno);
                if (r < 0)
                        return log_error_errno(r, "Failed to determine block device of /usr file system: %m");
                else if (r == 0) {
                        log_debug("Neither root nor /usr file system are on a (single) block device.");
                        return 0;
                }
        }

        return enumerate_partitions(devno);
}

int main(int argc, char *argv[]) {
        int r = 0;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[3];

        log_set_prohibit_ipc(true);
        log_set_target(LOG_TARGET_AUTO);

        log_parse_environment();
        log_open();

        umask(0022);

        if (detect_container() > 0) {
                log_debug("In a container, exiting.");
                return EXIT_SUCCESS;
        }

        if (is_efi_boot()) {
                log_debug("EFI boot, leave swap auto detection to gpt-auto generator.");
                return 0;
        }

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, 0);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (!arg_enabled) {
                log_debug("Disabled, exiting.");
                return EXIT_SUCCESS;
        }

        if (!in_initrd()) {
                int k;

                k = add_mounts();
                if (k < 0)
                        r = k;
        }

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
