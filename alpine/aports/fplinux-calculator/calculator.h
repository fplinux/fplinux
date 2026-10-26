/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_CALCULATOR_H
#define FPLINUX_CALCULATOR_H

#include <stdbool.h>
#include <stddef.h>

/* Digits of one entered number; results keep as many significant digits. */
#define FPLINUX_CALCULATOR_DIGIT_COUNT 12
/* Sign, 12 digits and a separator; results are shortened to fit as well. */
#define FPLINUX_CALCULATOR_TEXT_CHARS 14
#define FPLINUX_CALCULATOR_TEXT_BYTES (FPLINUX_CALCULATOR_TEXT_CHARS + 1)
#define FPLINUX_CALCULATOR_SAVE_BYTES 64

enum fplinux_calculator_state {
	/* 0 is shown and there is nothing to delete. */
	FPLINUX_CALCULATOR_EMPTY,
	FPLINUX_CALCULATOR_FIRST_NUMBER,
	/* An operator is pending; the second number may be in progress. */
	FPLINUX_CALCULATOR_OPERATION,
	FPLINUX_CALCULATOR_RESULT,
	/* Division by zero or overflow; the application names the error. */
	FPLINUX_CALCULATOR_ERROR,
};

enum fplinux_calculator_error {
	FPLINUX_CALCULATOR_NO_ERROR,
	FPLINUX_CALCULATOR_DIVIDE_BY_ZERO,
	FPLINUX_CALCULATOR_OVERFLOW,
};

enum fplinux_calculator_input_result {
	FPLINUX_CALCULATOR_IGNORED,
	FPLINUX_CALCULATOR_CHANGED,
	FPLINUX_CALCULATOR_DIGIT_LIMIT,
};

/*
 * A keypad calculator: operations apply from left to right without
 * precedence. Numbers use '.' internally; display text takes the caller's
 * decimal separator.
 */
struct fplinux_calculator {
	/* Number being typed: an optional '-', digits and at most one '.'. */
	char entry[FPLINUX_CALCULATOR_TEXT_BYTES];
	/* First operand of the pending operation, or the result shown. */
	double accumulator;
	/* Pending '+', '-', '*' or '/', or '\0'. */
	char operation;
	bool result;
	enum fplinux_calculator_error error;
};

/*
 * Keys: '0'-'9', '.' decimal separator, '#' change sign, '+', '-', '*', '/',
 * '=' equals, '<' delete one character, 'C' clear all. In the error state a
 * digit or '.' starts a new number and every other key only clears. Returns
 * IGNORED when the key changed nothing, or DIGIT_LIMIT when a new digit or
 * separator cannot fit. Repeated zero or separator is ignored.
 */
enum fplinux_calculator_input_result
fplinux_calculator_input(struct fplinux_calculator *calculator, char key);
enum fplinux_calculator_state
fplinux_calculator_get_state(const struct fplinux_calculator *calculator);
/* True when '=' computes: an operator is pending and a second number typed. */
bool fplinux_calculator_can_evaluate(
	const struct fplinux_calculator *calculator);
/* The number being typed, else the result or first operand; "" on error. */
void fplinux_calculator_display(const struct fplinux_calculator *calculator,
				char separator, char *text, size_t capacity);
/*
 * At most 12 significant digits without trailing zeros, exponent form
 * "1.5e15" from 1e12 or below 1e-4, shortened to 14 characters.
 */
void fplinux_calculator_format(double value, char separator, char *text,
			       size_t capacity);
/* Storage text for fplinux_calculator_restore(). */
void fplinux_calculator_save(const struct fplinux_calculator *calculator,
			     char *text, size_t capacity);
/* Restores saved text; anything else clears the calculator and fails. */
bool fplinux_calculator_restore(struct fplinux_calculator *calculator,
				const char *text);

#endif
