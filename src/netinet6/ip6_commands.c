/*
 * IPv6 Commands
 *
 * Copyright (c) 2018, AT&T Intellectual Property.  All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <getopt.h>

#include "commands.h"
#include "ip6_funcs.h"

int cmd_ip6(FILE *f, int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "redirects")) {
		bool enable = !strcmp(argv[2], "enable");

		ip6_redirects_set(enable);
		return 0;
	}
	fprintf(f, "ip6 command invalid\n");
	return -1;
}
