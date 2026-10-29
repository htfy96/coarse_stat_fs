// SPDX-License-Identifier: GPL-2.0
/* Registers the calling thread, reports readiness, then waits to be killed. */
#include "t.h"

int main(void)
{
	struct csf_snap s;

	if (csf_thread_snap(&s))
		DIE("register");
	printf("ready\n");
	fflush(stdout);
	pause();
	return 0;
}
