// SPDX-License-Identifier: GPL-2.0-only
#include <limits.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	volatile int value = INT_MAX;

	(void)argv;
	if (argc > 1)
		close(STDERR_FILENO);
	return value + 1;
}
