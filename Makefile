#
# Copyright (C) 2003-2004 Greg Kroah-Hartman <greg@kroah.com>
# Copyright (C) 2004-2006 Kay Sievers <kay.sievers@vrfy.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#

VERSION = 105

# set this to make use of syslog
USE_LOG = true

# compile-in development debug messages
# (export UDEV_LOG="debug" or set udev_log="debug" in udev.conf
#  to print the debug messages to syslog)
DEBUG = false

# compile with gcc's code coverage option
USE_GCOV = false

# include Security-Enhanced Linux support
USE_SELINUX = false

# set this to create statically linked binaries
USE_STATIC = false

# to build any of the extras programs pass:
#  make EXTRAS="extras/<extra1> extras/<extra2>"
EXTRAS =

# make the build silent
V =

PROGRAMS = \
	udevd				\
	udevtrigger			\
	udevsettle			\
	udevcontrol			\
	udevmonitor			\
	udevinfo			\
	udevtest			\
	test-udev			\
	udevstart

HEADERS = \
	udev.h				\
	udevd.h				\
	udev_rules.h			\
	logging.h			\
	udev_sysdeps.h			\
	udev_selinux.h			\
	list.h

UDEV_OBJS = \
	udev_device.o			\
	udev_config.o			\
	udev_node.o			\
	udev_db.o			\
	udev_sysfs.o			\
	udev_rules.o			\
	udev_rules_parse.o		\
	udev_utils.o			\
	udev_utils_string.o		\
	udev_utils_file.o		\
	udev_utils_run.o		\
	udev_sysdeps.o
LIBUDEV = libudev.a

MAN_PAGES = \
	udev.7				\
	udevmonitor.8			\
	udevd.8				\
	udevtrigger.8			\
	udevsettle.8			\
	udevtest.8			\
	udevinfo.8			\
	udevstart.8

GEN_HEADERS = \
	udev_version.h

prefix ?=
etcdir =	${prefix}/etc
sbindir =	${prefix}/sbin
usrbindir =	${prefix}/usr/bin
usrsbindir =	${prefix}/usr/sbin
libudevdir =	${prefix}/lib/udev
mandir =	${prefix}/usr/share/man
configdir =	${etcdir}/udev
udevdir =	/dev
DESTDIR =

INSTALL = install -c
INSTALL_PROGRAM = ${INSTALL}
INSTALL_DATA = ${INSTALL} -m 644
INSTALL_SCRIPT = ${INSTALL}
PWD = $(shell pwd)

CROSS_COMPILE ?=
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
RANLIB = $(CROSS_COMPILE)ranlib

CFLAGS		= -g -Wall -pipe -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
WARNINGS	= -Wstrict-prototypes -Wsign-compare -Wshadow \
		  -Wchar-subscripts -Wmissing-declarations -Wnested-externs \
		  -Wpointer-arith -Wcast-align -Wsign-compare -Wmissing-prototypes
CFLAGS		+= $(WARNINGS)

LDFLAGS = -Wl,-warn-common

OPTFLAGS = -Os
CFLAGS += $(OPTFLAGS)

ifeq ($(strip $(USE_LOG)),true)
	CFLAGS += -DUSE_LOG
endif

# if DEBUG is enabled, then we do not strip
ifeq ($(strip $(DEBUG)),true)
	CFLAGS  += -DDEBUG
endif

ifeq ($(strip $(USE_GCOV)),true)
	CFLAGS += -fprofile-arcs -ftest-coverage
	LDFLAGS += -fprofile-arcs
endif

ifeq ($(strip $(USE_SELINUX)),true)
	UDEV_OBJS += udev_selinux.o
	LIB_OBJS += -lselinux -lsepol
	CFLAGS += -DUSE_SELINUX
endif

ifeq ($(strip $(USE_STATIC)),true)
	CFLAGS += -DUSE_STATIC
	LDFLAGS += -static
endif

ifeq ($(strip $(V)),)
	E = @echo
	Q = @
else
	E = @\#
	Q =
endif
export E Q

all: $(PROGRAMS) $(MAN_PAGES)
	$(Q) extras="$(EXTRAS)"; for target in $$extras; do \
		$(MAKE) CC="$(CC)" \
			CFLAGS="$(CFLAGS)" \
			LD="$(LD)" \
			LDFLAGS="$(LDFLAGS)" \
			AR="$(AR)" \
			RANLIB="$(RANLIB)" \
			LIB_OBJS="$(LIB_OBJS)" \
			LIBUDEV="$(PWD)/$(LIBUDEV)" \
			-C $$target $@ || exit 1; \
	done;
.PHONY: all
.DEFAULT: all

# clear implicit rules
.SUFFIXES:

# build the objects
%.o: %.c $(HEADERS) $(GEN_HEADERS)
	$(E) "  CC      " $@
	$(Q) $(CC) -c $(CFLAGS) $< -o $@

# "Static Pattern Rule" to build all programs
$(PROGRAMS): %: $(HEADERS) $(GEN_HEADERS) $(LIBUDEV) %.o
	$(E) "  LD      " $@
	$(Q) $(LD) $(LDFLAGS) $@.o -o $@ $(LIBUDEV) $(LIB_OBJS)

$(LIBUDEV): $(HEADERS) $(GEN_HEADERS) $(UDEV_OBJS)
	$(Q) rm -f $@
	$(E) "  AR      " $@
	$(Q) $(AR) cq $@ $(UDEV_OBJS)
	$(E) "  RANLIB  " $@
	$(Q) $(RANLIB) $@

udev_version.h:
	$(E) "  GENHDR  " $@
	$(Q) echo "/* Generated by make. */" > $@
	$(Q) echo \#define UDEV_VERSION		\"$(VERSION)\" >> $@
	$(Q) echo \#define UDEV_ROOT		\"$(udevdir)\" >> $@
	$(Q) echo \#define UDEV_CONFIG_FILE	\"$(configdir)/udev.conf\" >> $@
	$(Q) echo \#define UDEV_RULES_DIR	\"$(configdir)/rules.d\" >> $@

# man pages
%.8 %.7: %.xml
	$(E) "  XMLTO   " $@
	$(Q) xmlto man $?
.PRECIOUS: %.8

clean:
	$(E) "  CLEAN   "
	$(Q) - find . -type f -name '*.orig' -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name '*.rej' -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name '*~' -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name '*.[oas]' -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name "*.gcno" -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name "*.gcda" -print0 | xargs -0r rm -f
	$(Q) - find . -type f -name "*.gcov" -print0 | xargs -0r rm -f
	$(Q) - rm -f udev_gcov.txt
	$(Q) - rm -f core $(PROGRAMS) $(GEN_HEADERS)
	$(Q) - rm -f udev-$(VERSION).tar.gz
	$(Q) - rm -f udev-$(VERSION).tar.bz2
	@ extras="$(EXTRAS)"; for target in $$extras; do \
		$(MAKE) -C $$target $@ || exit 1; \
	done;
.PHONY: clean

release:
	git-archive --format=tar --prefix=udev-$(VERSION)/ HEAD | gzip -9v > udev-$(VERSION).tar.gz
	git-archive --format=tar --prefix=udev-$(VERSION)/ HEAD | bzip2 -9v > udev-$(VERSION).tar.bz2
.PHONY: release

install-config:
	$(INSTALL) -d $(DESTDIR)$(configdir)/rules.d
	@ if [ ! -r $(DESTDIR)$(configdir)/udev.conf ]; then \
		$(INSTALL_DATA) etc/udev/udev.conf $(DESTDIR)$(configdir); \
	fi
	@ if [ ! -r $(DESTDIR)$(configdir)/rules.d/50-udev.rules ]; then \
		echo; \
		echo "pick a udev rules file from the etc/udev directory that matches your distribution"; \
		echo; \
	fi
	@ extras="$(EXTRAS)"; for target in $$extras; do \
		$(MAKE) -C $$target $@ || exit 1; \
	done;
.PHONY: install-config

install-man:
	$(INSTALL_DATA) -D udev.7 $(DESTDIR)$(mandir)/man7/udev.7
	$(INSTALL_DATA) -D udevinfo.8 $(DESTDIR)$(mandir)/man8/udevinfo.8
	$(INSTALL_DATA) -D udevtest.8 $(DESTDIR)$(mandir)/man8/udevtest.8
	$(INSTALL_DATA) -D udevd.8 $(DESTDIR)$(mandir)/man8/udevd.8
	$(INSTALL_DATA) -D udevtrigger.8 $(DESTDIR)$(mandir)/man8/udevtrigger.8
	$(INSTALL_DATA) -D udevsettle.8 $(DESTDIR)$(mandir)/man8/udevsettle.8
	$(INSTALL_DATA) -D udevmonitor.8 $(DESTDIR)$(mandir)/man8/udevmonitor.8
	- ln -f -s udevd.8 $(DESTDIR)$(mandir)/man8/udevcontrol.8
	@extras="$(EXTRAS)"; for target in $$extras; do \
		$(MAKE) -C $$target $@ || exit 1; \
	done;
.PHONY: install-man

uninstall-man:
	- rm -f $(DESTDIR)$(mandir)/man7/udev.7
	- rm -f $(DESTDIR)$(mandir)/man8/udevinfo.8
	- rm -f $(DESTDIR)$(mandir)/man8/udevtest.8
	- rm -f $(DESTDIR)$(mandir)/man8/udevd.8
	- rm -f $(DESTDIR)$(mandir)/man8/udevtrigger.8
	- rm -f $(DESTDIR)$(mandir)/man8/udevsettle.8
	- rm -f $(DESTDIR)$(mandir)/man8/udevmonitor.8
	- rm -f $(DESTDIR)$(mandir)/man8/udevcontrol.8
	@ extras="$(EXTRAS)"; for target in $$extras; do \
		$(MAKE) -C $$target $@ || exit 1; \
	done;
.PHONY: uninstall-man

install-bin:
	$(INSTALL) -d $(DESTDIR)$(udevdir)
	$(INSTALL_PROGRAM) -D udevd $(DESTDIR)$(sbindir)/udevd
	$(INSTALL_PROGRAM) -D udevtrigger $(DESTDIR)$(sbindir)/udevtrigger
	$(INSTALL_PROGRAM) -D udevsettle $(DESTDIR)$(sbindir)/udevsettle
	$(INSTALL_PROGRAM) -D udevcontrol $(DESTDIR)$(sbindir)/udevcontrol
	$(INSTALL_PROGRAM) -D udevmonitor $(DESTDIR)$(usrsbindir)/udevmonitor
	$(INSTALL_PROGRAM) -D udevinfo $(DESTDIR)$(usrbindir)/udevinfo
	$(INSTALL_PROGRAM) -D udevtest $(DESTDIR)$(usrbindir)/udevtest
	@extras="$(EXTRAS)"; for target in $$extras; do \
		$(MAKE) -C $$target $@ || exit 1; \
	done;
ifndef DESTDIR
	- killall udevd
	- rm -rf /dev/.udev
	- $(sbindir)/udevd --daemon
endif
.PHONY: install-bin

uninstall-bin:
	- rm -f $(DESTDIR)$(sbindir)/udevd
	- rm -f $(DESTDIR)$(sbindir)/udevtrigger
	- rm -f $(DESTDIR)$(sbindir)/udevsettle
	- rm -f $(DESTDIR)$(sbindir)/udevcontrol
	- rm -f $(DESTDIR)$(usrsbindir)/udevmonitor
	- rm -f $(DESTDIR)$(usrbindir)/udevinfo
	- rm -f $(DESTDIR)$(usrbindir)/udevtest
ifndef DESTDIR
	- killall udevd
	- rm -rf /dev/.udev
endif
	@extras="$(EXTRAS)"; for target in $$extras; do \
		$(MAKE) -C $$target $@ || exit 1; \
	done;
.PHONY: uninstall-bin

install: all install-bin install-config install-man
.PHONY: install

uninstall: uninstall-bin uninstall-man
.PHONY: uninstall

test tests: all
	@ cd test && ./udev-test.pl
	@ cd test && ./udevstart-test.pl
.PHONY: test tests

buildtest:
	test/simple-build-check.sh
.PHONY: buildtest

ChangeLog: Makefile
	@ mv $@ $@.tmp
	@ echo "Summary of changes from v$(shell echo $$(($(VERSION) - 1))) to v$(VERSION)" >> $@
	@ echo "============================================" >> $@
	@ echo >> $@
	@ git log --pretty=short $(shell echo $$(($(VERSION) - 1)))..HEAD | git shortlog  >> $@
	@ echo >> $@
	@ cat $@
	@ cat $@.tmp >> $@
	@ rm $@.tmp

gcov-all:
	$(MAKE) clean all USE_GCOV=true
	@ echo
	@ echo "binaries built with gcov support."
	@ echo "run the tests and analyze with 'make udev_gcov.txt'"
.PHONY: gcov-all

# see docs/README-gcov_for_udev
udev_gcov.txt: $(wildcard *.gcda) $(wildcard *.gcno)
	for file in `find -maxdepth 1 -name "*.gcno"`; do \
		name=`basename $$file .gcno`; \
		echo "################" >> $@; \
		echo "$$name.c" >> $@; \
		echo "################" >> $@; \
		if [ -e "$$name.gcda" ]; then \
			gcov -l "$$name.c" >> $@ 2>&1; \
		else \
			echo "code for $$name.c was never executed" >> $@ 2>&1; \
		fi; \
		echo >> $@; \
	done; \
	echo "view $@ for the result"
