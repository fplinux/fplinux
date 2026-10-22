/* SPDX-License-Identifier: GPL-2.0-only */
#include "calculator.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void type(struct fplinux_calculator *calculator, const char *keys)
{
	for (; *keys; keys++)
		fplinux_calculator_input(calculator, *keys);
}

static void expect_display(const struct fplinux_calculator *calculator,
			   char separator, const char *expected)
{
	char text[64];

	fplinux_calculator_display(calculator, separator, text, sizeof(text));
	assert(!strcmp(text, expected));
}

static void arithmetic(void)
{
	static const struct {
		const char *keys;
		const char *expected;
	} cases[] = {
		{ "2+3=", "5" },	 { "2+3*4=", "20" },
		{ "20/4-3=", "2" },	 { "1.5#+2.25=", "0.75" },
		{ "0.1+0.2-0.3=", "0" }, { "5+#2=", "3" },
		{ "7+-2=", "5" },
	};

	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
	     index++) {
		struct fplinux_calculator calculator = { 0 };

		type(&calculator, cases[index].keys);
		expect_display(&calculator, '.', cases[index].expected);
		assert(fplinux_calculator_get_state(&calculator) ==
		       FPLINUX_CALCULATOR_RESULT);
		assert(!fplinux_calculator_can_evaluate(&calculator));
	}
}

static void decimal_and_sign(void)
{
	struct fplinux_calculator calculator = { 0 };

	type(&calculator, ".25#");
	expect_display(&calculator, ',', "-0,25");
	assert(!fplinux_calculator_input(&calculator, '.'));
	type(&calculator, "*4=");
	expect_display(&calculator, '.', "-1");
	type(&calculator, "#");
	expect_display(&calculator, '.', "1");
	type(&calculator, "9");
	expect_display(&calculator, '.', "9");
	assert(fplinux_calculator_get_state(&calculator) ==
	       FPLINUX_CALCULATOR_FIRST_NUMBER);
}

static void divide_by_zero_and_recovery(void)
{
	struct fplinux_calculator calculator = { 0 };

	type(&calculator, "8/0=");
	assert(fplinux_calculator_get_state(&calculator) ==
	       FPLINUX_CALCULATOR_ERROR);
	expect_display(&calculator, '.', "");
	assert(!fplinux_calculator_can_evaluate(&calculator));
	type(&calculator, "4+2=");
	expect_display(&calculator, '.', "6");
	type(&calculator, "/0=+");
	assert(fplinux_calculator_get_state(&calculator) ==
	       FPLINUX_CALCULATOR_EMPTY);
	expect_display(&calculator, '.', "0");
	type(&calculator, "1/0=.5*8=");
	expect_display(&calculator, '.', "4");
}

static void overflow_and_recovery(void)
{
	struct fplinux_calculator calculator = { 0 };

	type(&calculator, "999999999999*");
	for (unsigned int index = 0; index < 24; index++)
		type(&calculator, "999999999999*");
	type(&calculator, "999999999999=");
	assert(fplinux_calculator_get_state(&calculator) ==
	       FPLINUX_CALCULATOR_ERROR);
	type(&calculator, "2+3=");
	expect_display(&calculator, '.', "5");
}

static void limits_delete_and_reset(void)
{
	struct fplinux_calculator calculator = { 0 };

	type(&calculator, "12345678901.2#");
	expect_display(&calculator, '.', "-12345678901.2");
	assert(!fplinux_calculator_input(&calculator, '3'));
	expect_display(&calculator, '.', "-12345678901.2");
	type(&calculator, "<3");
	expect_display(&calculator, '.', "-12345678901.3");
	type(&calculator, "C12+<3");
	expect_display(&calculator, '.', "123");
	assert(!fplinux_calculator_can_evaluate(&calculator));
	type(&calculator, "+4");
	assert(fplinux_calculator_can_evaluate(&calculator));
	type(&calculator, "=C");
	expect_display(&calculator, '.', "0");
	assert(!fplinux_calculator_input(&calculator, '<'));
	assert(!fplinux_calculator_input(&calculator, 'C'));
	assert(!fplinux_calculator_input(&calculator, 'q'));
	type(&calculator, "00");
	expect_display(&calculator, '.', "0");
	assert(!fplinux_calculator_input(&calculator, '0'));
}

static void saved_calculation_continues(void)
{
	struct fplinux_calculator original = { 0 };
	struct fplinux_calculator restored;
	char saved[128];

	type(&original, "12.5+3.");
	fplinux_calculator_save(&original, saved, sizeof(saved));
	assert(fplinux_calculator_restore(&restored, saved));
	type(&restored, "25=");
	expect_display(&restored, '.', "15.75");
	fplinux_calculator_save(&restored, saved, sizeof(saved));
	assert(fplinux_calculator_restore(&original, saved));
	type(&original, "*4=");
	expect_display(&original, '.', "63");
	type(&original, "/0=");
	fplinux_calculator_save(&original, saved, sizeof(saved));
	assert(fplinux_calculator_restore(&restored, saved));
	assert(fplinux_calculator_get_state(&restored) ==
	       FPLINUX_CALCULATOR_ERROR);
	type(&restored, "7-2=");
	expect_display(&restored, '.', "5");
}

static void number_formatting(void)
{
	static const struct {
		double value;
		const char *expected;
	} cases[] = {
		{ -0.0, "0" },
		{ 123456789012.0, "123456789012" },
		{ 1500000000000000.0, "1.5e15" },
		{ 0.00000015, "1.5e-7" },
		{ -1234567890120000000000000000000.0, "-1.23456789e30" },
	};

	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]);
	     index++) {
		char text[64];

		fplinux_calculator_format(cases[index].value, '.', text,
					  sizeof(text));
		assert(!strcmp(text, cases[index].expected));
		assert(strlen(text) <= 14);
	}
}

int main(void)
{
	arithmetic();
	decimal_and_sign();
	divide_by_zero_and_recovery();
	overflow_and_recovery();
	limits_delete_and_reset();
	saved_calculation_continues();
	number_formatting();
	puts("calculator-model: arithmetic, editing and saved continuation passed");
	return 0;
}
