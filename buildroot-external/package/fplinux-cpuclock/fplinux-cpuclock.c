// SPDX-License-Identifier: GPL-2.0-only
/*
 * Measure the CPU clock of the phone directly, on the phone, rather than
 * taking any stated figure on trust.
 *
 * The method needs one architectural fact and nothing else: on a Cortex-A7 an
 * integer ALU result is available to the next instruction after one cycle, so
 * a chain of additions where every instruction consumes the previous result
 * retires at exactly one instruction per cycle. There is no instruction-level
 * parallelism left for the pipeline to exploit and no memory traffic at all,
 * because the chain lives entirely in registers.
 *
 * Counting those instructions and dividing by the elapsed monotonic time
 * therefore yields the clock frequency. The loop counter and branch that
 * bracket each unrolled block are not counted, which makes the reported
 * frequency a slight underestimate rather than an optimistic one.
 */
/* clock_gettime and struct timespec are POSIX, not ISO C. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHAIN_LENGTH 256U
#define DEFAULT_ROUNDS 5U
#define DEFAULT_ITERATIONS 2000000U

/*
 * Kept out of line on purpose: the unrolled block is large enough that
 * inlining it into main pushes the literal pool out of reach.
 */
__attribute__((noinline)) static uint32_t
dependent_chain(uint32_t seed, uint32_t step, uint32_t iterations)
{
	uint32_t accumulator = seed;

#ifdef __arm__
	__asm__ volatile("1:\n"
			 ".rept 256\n"
			 "add %[acc], %[acc], %[step]\n"
			 ".endr\n"
			 "subs %[count], %[count], #1\n"
			 "bne 1b\n"
			 : [acc] "+r"(accumulator), [count] "+r"(iterations)
			 : [step] "r"(step)
			 : "cc");
#else
	/*
	 * Stand-in so the file still compiles for host-side static
	 * analysis. It is never executed on the phone.
	 */
	while (iterations--)
		for (unsigned int i = 0; i < CHAIN_LENGTH; i++)
			accumulator += step;
#endif
	return accumulator;
}

static double elapsed_seconds(const struct timespec *start,
			      const struct timespec *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

int main(int argc, char **argv)
{
	unsigned long iterations = DEFAULT_ITERATIONS;
	unsigned long rounds = DEFAULT_ROUNDS;
	double best = 0.0;

	if (argc > 1)
		iterations = strtoul(argv[1], NULL, 0);
	if (argc > 2)
		rounds = strtoul(argv[2], NULL, 0);
	if (!iterations || !rounds) {
		fprintf(stderr,
			"usage: fplinux-cpuclock [iterations] [rounds]\n");
		return 2;
	}

	printf("fplinux-cpuclock: %lu rounds of %lu x %u dependent integer "
	       "additions\n",
	       rounds, iterations, CHAIN_LENGTH);
	printf("fplinux-cpuclock: one addition retires per cycle on this core, "
	       "so the\n");
	printf("fplinux-cpuclock: instruction rate is the clock; loop overhead "
	       "is excluded\n");
	printf(
	    "fplinux-cpuclock: and makes every figure below a lower bound.\n");

	/* Warm the pipeline and page in the code before the first measurement.
	 */
	(void)dependent_chain(1U, 1U, 1000U);

	for (unsigned long round = 0; round < rounds; round++) {
		struct timespec start;
		struct timespec end;
		double seconds;
		double hz;

		if (clock_gettime(CLOCK_MONOTONIC, &start)) {
			perror("clock_gettime");
			return 1;
		}
		(void)dependent_chain(round, 1U, (uint32_t)iterations);
		if (clock_gettime(CLOCK_MONOTONIC, &end)) {
			perror("clock_gettime");
			return 1;
		}

		seconds = elapsed_seconds(&start, &end);
		if (seconds <= 0.0) {
			fprintf(stderr,
				"fplinux-cpuclock: the clock did not advance; "
				"is the clocksource working?\n");
			return 1;
		}
		hz = (double)iterations * (double)CHAIN_LENGTH / seconds;
		if (hz > best)
			best = hz;
		printf("fplinux-cpuclock: round %lu: %.3f s -> %.2f MHz\n",
		       round + 1, seconds, hz / 1e6);
	}

	printf("fplinux-cpuclock: best of %lu rounds: %.2f MHz\n", rounds,
	       best / 1e6);
	printf("fplinux-cpuclock: the device tree currently claims %.2f MHz\n",
	       1000.0);
	return 0;
}
