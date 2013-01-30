/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

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

#include <string.h>

#include "util.h"
#include "specifier.h"

static void test_specifier_printf(void) {
        char *w;

        const Specifier table[] = {
                { 'a', specifier_string, (char*) "AAAA" },
                { 'b', specifier_string, (char*) "BBBB" },
                { 0, NULL, NULL }
        };

        w = specifier_printf("xxx a=%a b=%b yyy", table, NULL);
        printf("<%s>\n", w);
        free(w);
}

static void test_foreach_word_quoted(void) {
        char *w, *state;
        size_t l;
        const char test[] = "test a b c 'd' e '' '' hhh '' ''";
        printf("<%s>\n", test);
        FOREACH_WORD_QUOTED(w, l, test, state) {
                char *t;

                assert_se(t = strndup(w, l));
                printf("<%s>\n", t);
                free(t);
        }
}

static void test_default_term_for_tty(void) {
        printf("%s\n", default_term_for_tty("/dev/tty23"));
        printf("%s\n", default_term_for_tty("/dev/ttyS23"));
        printf("%s\n", default_term_for_tty("/dev/tty0"));
        printf("%s\n", default_term_for_tty("/dev/pty0"));
        printf("%s\n", default_term_for_tty("/dev/pts/0"));
        printf("%s\n", default_term_for_tty("/dev/console"));
        printf("%s\n", default_term_for_tty("tty23"));
        printf("%s\n", default_term_for_tty("ttyS23"));
        printf("%s\n", default_term_for_tty("tty0"));
        printf("%s\n", default_term_for_tty("pty0"));
        printf("%s\n", default_term_for_tty("pts/0"));
        printf("%s\n", default_term_for_tty("console"));
}

int main(int argc, char *argv[]) {
        test_default_term_for_tty();
        test_foreach_word_quoted();
        test_specifier_printf();

        return 0;
}
