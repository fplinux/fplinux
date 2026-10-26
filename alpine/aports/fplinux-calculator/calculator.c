/* SPDX-License-Identifier: GPL-2.0-only */
#include "calculator.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t count_digits(const char *text)
{
	size_t count = 0;

	for (; *text; text++)
		count += *text >= '0' && *text <= '9';
	return count;
}

static void clear(struct fplinux_calculator *calculator)
{
	*calculator = (struct fplinux_calculator){ 0 };
}

static void fail(struct fplinux_calculator *calculator,
		 enum fplinux_calculator_error error)
{
	clear(calculator);
	calculator->error = error;
}

/* Results keep 12 significant digits, so 0.1 + 0.2 - 0.3 gives 0. */
static double round_digits(double value)
{
	char text[32];

	snprintf(text, sizeof(text), "%.*g", FPLINUX_CALCULATOR_DIGIT_COUNT,
		 value);
	value = strtod(text, NULL);
	return value == 0 ? 0 : value;
}

/* Applies the pending operation to the typed number; false on an error. */
static bool calculate(struct fplinux_calculator *calculator)
{
	double operand = strtod(calculator->entry, NULL);
	double value = calculator->accumulator;

	switch (calculator->operation) {
	case '+':
		value += operand;
		break;
	case '-':
		value -= operand;
		break;
	case '*':
		value *= operand;
		break;
	default:
		if (operand == 0) {
			fail(calculator, FPLINUX_CALCULATOR_DIVIDE_BY_ZERO);
			return false;
		}
		value /= operand;
	}
	value = round_digits(value);
	if (!isfinite(value)) {
		fail(calculator, FPLINUX_CALCULATOR_OVERFLOW);
		return false;
	}
	calculator->accumulator = value;
	calculator->entry[0] = '\0';
	return true;
}

static enum fplinux_calculator_input_result
type_digit(struct fplinux_calculator *calculator, char digit)
{
	char *entry = calculator->entry;
	size_t length;

	if (calculator->result)
		clear(calculator);
	length = strlen(entry);
	if (!strcmp(entry, "0") || !strcmp(entry, "-0")) {
		if (digit == '0')
			return FPLINUX_CALCULATOR_IGNORED;
		entry[length - 1] = digit;
		return FPLINUX_CALCULATOR_CHANGED;
	}
	if (count_digits(entry) >= FPLINUX_CALCULATOR_DIGIT_COUNT)
		return FPLINUX_CALCULATOR_DIGIT_LIMIT;
	entry[length] = digit;
	entry[length + 1] = '\0';
	return FPLINUX_CALCULATOR_CHANGED;
}

static enum fplinux_calculator_input_result
type_separator(struct fplinux_calculator *calculator)
{
	char *entry = calculator->entry;
	size_t digits;

	if (calculator->result)
		clear(calculator);
	digits = count_digits(entry);
	if (strchr(entry, '.'))
		return FPLINUX_CALCULATOR_IGNORED;
	if (digits >= FPLINUX_CALCULATOR_DIGIT_COUNT)
		return FPLINUX_CALCULATOR_DIGIT_LIMIT;
	strcat(entry, digits ? "." : "0.");
	return FPLINUX_CALCULATOR_CHANGED;
}

static bool change_sign(struct fplinux_calculator *calculator)
{
	char *entry = calculator->entry;

	if (calculator->result) {
		if (calculator->accumulator == 0)
			return false;
		calculator->accumulator = -calculator->accumulator;
		return true;
	}
	if (entry[0] == '-') {
		memmove(entry, entry + 1, strlen(entry));
	} else {
		memmove(entry + 1, entry, strlen(entry) + 1);
		entry[0] = '-';
	}
	return true;
}

static bool choose_operation(struct fplinux_calculator *calculator,
			     char operation)
{
	if (count_digits(calculator->entry)) {
		if (!calculator->operation)
			calculator->accumulator =
				round_digits(strtod(calculator->entry, NULL));
		else if (!calculate(calculator))
			return true;
	} else if (!calculator->operation && !calculator->result) {
		calculator->accumulator = 0;
	} else if (calculator->operation == operation &&
		   !calculator->entry[0]) {
		return false;
	}
	/* A lone '-' without digits is dropped with the replaced operator. */
	calculator->entry[0] = '\0';
	calculator->result = false;
	calculator->operation = operation;
	return true;
}

static bool evaluate(struct fplinux_calculator *calculator)
{
	if (!fplinux_calculator_can_evaluate(calculator))
		return false;
	if (calculate(calculator)) {
		calculator->operation = '\0';
		calculator->result = true;
	}
	return true;
}

/* Without its operator the first number becomes editable again. */
static void edit_accumulator(struct fplinux_calculator *calculator)
{
	char text[32];
	size_t length;

	calculator->operation = '\0';
	if (calculator->accumulator == 0)
		return;
	snprintf(text, sizeof(text), "%.*g", FPLINUX_CALCULATOR_DIGIT_COUNT,
		 calculator->accumulator);
	length = strlen(text);
	if (strchr(text, 'e') ||
	    count_digits(text) > FPLINUX_CALCULATOR_DIGIT_COUNT ||
	    length >= sizeof(calculator->entry)) {
		calculator->result = true;
		return;
	}
	memcpy(calculator->entry, text, length + 1);
	calculator->accumulator = 0;
}

static bool delete_character(struct fplinux_calculator *calculator)
{
	size_t length = strlen(calculator->entry);

	if (calculator->result) {
		clear(calculator);
		return true;
	}
	if (length) {
		calculator->entry[length - 1] = '\0';
		return true;
	}
	if (!calculator->operation)
		return false;
	edit_accumulator(calculator);
	return true;
}

enum fplinux_calculator_input_result
fplinux_calculator_input(struct fplinux_calculator *calculator, char key)
{
	bool starts_number = (key >= '0' && key <= '9') || key == '.';
	bool changed;

	if (!key || !strchr("0123456789.#+-*/=<C", key))
		return FPLINUX_CALCULATOR_IGNORED;
	if ((calculator->error && !starts_number) || key == 'C') {
		changed = fplinux_calculator_get_state(calculator) !=
			  FPLINUX_CALCULATOR_EMPTY;

		clear(calculator);
		return changed ? FPLINUX_CALCULATOR_CHANGED :
				 FPLINUX_CALCULATOR_IGNORED;
	}
	/* After an error a digit or separator starts a new number. */
	if (calculator->error)
		clear(calculator);
	if (key >= '0' && key <= '9')
		return type_digit(calculator, key);
	switch (key) {
	case '.':
		return type_separator(calculator);
	case '#':
		changed = change_sign(calculator);
		break;
	case '=':
		changed = evaluate(calculator);
		break;
	case '<':
		changed = delete_character(calculator);
		break;
	default:
		changed = choose_operation(calculator, key);
		break;
	}
	return changed ? FPLINUX_CALCULATOR_CHANGED :
			 FPLINUX_CALCULATOR_IGNORED;
}

enum fplinux_calculator_state
fplinux_calculator_get_state(const struct fplinux_calculator *calculator)
{
	if (calculator->error)
		return FPLINUX_CALCULATOR_ERROR;
	if (calculator->result)
		return FPLINUX_CALCULATOR_RESULT;
	if (calculator->operation)
		return FPLINUX_CALCULATOR_OPERATION;
	if (calculator->entry[0])
		return FPLINUX_CALCULATOR_FIRST_NUMBER;
	return FPLINUX_CALCULATOR_EMPTY;
}

bool fplinux_calculator_can_evaluate(const struct fplinux_calculator *calculator)
{
	return !calculator->error && calculator->operation &&
	       count_digits(calculator->entry);
}

static void use_separator(char *text, char separator)
{
	char *point = strchr(text, '.');

	if (point)
		*point = separator;
}

void fplinux_calculator_display(const struct fplinux_calculator *calculator,
				char separator, char *text, size_t capacity)
{
	if (calculator->error) {
		snprintf(text, capacity, "%s", "");
	} else if (calculator->entry[0]) {
		snprintf(text, capacity, "%s", calculator->entry);
		use_separator(text, separator);
	} else if (calculator->result || calculator->operation) {
		fplinux_calculator_format(calculator->accumulator, separator,
					  text, capacity);
	} else {
		snprintf(text, capacity, "%s", "0");
	}
}

/* "e+15" becomes "e15" and "e-07" becomes "e-7". */
static void shorten_exponent(char *text)
{
	char *digits = strchr(text, 'e');

	if (!digits)
		return;
	digits++;
	if (*digits == '+')
		memmove(digits, digits + 1, strlen(digits));
	else if (*digits == '-')
		digits++;
	while (digits[0] == '0' && digits[1])
		memmove(digits, digits + 1, strlen(digits));
}

void fplinux_calculator_format(double value, char separator, char *text,
			       size_t capacity)
{
	char buffer[32];
	int precision = FPLINUX_CALCULATOR_DIGIT_COUNT;

	/* Negative zero is shown as 0. */
	if (value == 0)
		value = 0;
	for (;;) {
		snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
		shorten_exponent(buffer);
		if (strlen(buffer) <= FPLINUX_CALCULATOR_TEXT_CHARS ||
		    precision == 1)
			break;
		precision--;
	}
	use_separator(buffer, separator);
	snprintf(text, capacity, "%s", buffer);
}

/* "<mode> <operation> <accumulator> <entry>": i, r, d or o; operation n. */
void fplinux_calculator_save(const struct fplinux_calculator *calculator,
			     char *text, size_t capacity)
{
	char mode = calculator->error == FPLINUX_CALCULATOR_DIVIDE_BY_ZERO ?
			    'd' :
		    calculator->error == FPLINUX_CALCULATOR_OVERFLOW ? 'o' :
		    calculator->result				     ? 'r' :
								       'i';

	snprintf(text, capacity, "%c %c %.17g %s", mode,
		 calculator->operation ? calculator->operation : 'n',
		 calculator->accumulator, calculator->entry);
}

bool fplinux_calculator_restore(struct fplinux_calculator *calculator,
				const char *text)
{
	struct fplinux_calculator restored = { 0 };
	const char *entry;
	char *end;
	double accumulator;
	size_t index;

	clear(calculator);
	if (strlen(text) < 5 || text[1] != ' ' || text[3] != ' ')
		return false;
	accumulator = strtod(text + 4, &end);
	if (end == text + 4 || (*end && *end != ' ') || !isfinite(accumulator))
		return false;
	entry = *end ? end + 1 : end;
	if (strlen(entry) > FPLINUX_CALCULATOR_TEXT_CHARS ||
	    strspn(entry, "0123456789.-") != strlen(entry))
		return false;
	if (text[0] == 'i' && strchr("+-*/n", text[2])) {
		restored.operation = text[2] == 'n' ? '\0' : text[2];
		restored.accumulator = restored.operation ? accumulator : 0;
		/* Typing the saved number again accepts only a valid entry. */
		for (index = 0; entry[index]; index++)
			fplinux_calculator_input(
				&restored,
				entry[index] == '-' ? '#' : entry[index]);
		if (strcmp(restored.entry, entry))
			return false;
	} else if (strchr("rdo", text[0]) && text[2] == 'n' && !entry[0]) {
		restored.result = text[0] == 'r';
		restored.error = text[0] == 'd' ?
					 FPLINUX_CALCULATOR_DIVIDE_BY_ZERO :
				 text[0] == 'o' ? FPLINUX_CALCULATOR_OVERFLOW :
						  FPLINUX_CALCULATOR_NO_ERROR;
		restored.accumulator = restored.result ? accumulator : 0;
	} else {
		return false;
	}
	*calculator = restored;
	return true;
}
