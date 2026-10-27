/* SPDX-License-Identifier: 0BSD */
/* Bounded vm_vsprintf for guest programs. Their va_list is a
 * BASE-AAPCS cursor in guest memory, never a host va_list: promoted scalars
 * occupy four bytes; double/long long occupy eight, aligned to eight. Include
 * after guest_data(), read32() and ascii_length(). All formats and buffers
 * follow the same syntax and guest-memory checks. */
#define GUEST_FORMAT_BYTES 2048u
#define GUEST_FORMAT_WIDTH 512u
#define GUEST_FORMAT_PRECISION 128u

static uint64_t guest_format_argument(uintptr_t *cursor, size_t bytes)
{
	uintptr_t aligned = *cursor;
	if (bytes == 8) {
		if (aligned > UINTPTR_MAX - 7)
			fail("guest format argument alignment overflow");
		aligned = (aligned + 7) & ~(uintptr_t)7;
	}
	if (aligned > UINTPTR_MAX - bytes ||
	    !guest_data((const void *)aligned, bytes, PF_R))
		fail("guest format argument bounds");
	const uint8_t *slot = (const uint8_t *)aligned;
	uint64_t value = read32(slot);
	if (bytes == 8)
		value |= (uint64_t)read32(slot + 4) << 32;
	*cursor = aligned + bytes;
	return value;
}

static unsigned guest_format_decimal(const char **text, unsigned limit)
{
	unsigned value = 0;
	while (**text >= '0' && **text <= '9') {
		unsigned digit = (unsigned)(*(*text)++ - '0');
		if (value > (limit - digit) / 10)
			fail("guest format width or precision exceeds bound");
		value = value * 10 + digit;
	}
	return value;
}

static int guest_format(char *out, const char *format, const void *args)
{
	/* Validate before staging; an invalid format never changes the output. */
	(void)ascii_length(format);
	char formatted[GUEST_FORMAT_BYTES];
	size_t used = 0;
	uintptr_t argument = (uintptr_t)args;
	if (argument & 3)
		fail("guest format va_list is not word aligned");
	for (const char *text = format; *text;) {
		if (*text != '%' || text[1] == '%') {
			if (used == sizeof(formatted) - 1)
				fail("guest format output exceeds bound");
			formatted[used++] = *text++;
			if (text[-1] == '%')
				++text;
			continue;
		}
		++text;
		enum { LEFT = 1, PLUS = 2, SPACE = 4, ALT = 8, ZERO = 16 };
		unsigned flags = 0;
		for (;;) {
			unsigned flag = *text == '-' ? LEFT :
					*text == '+' ? PLUS :
					*text == ' ' ? SPACE :
					*text == '#' ? ALT :
					*text == '0' ? ZERO :
						       0;
			if (!flag)
				break;
			flags |= flag;
			++text;
		}
		unsigned width =
			guest_format_decimal(&text, GUEST_FORMAT_WIDTH);
		int precision = -1;
		if (*text == '.') {
			++text;
			precision = (int)guest_format_decimal(
				&text, GUEST_FORMAT_PRECISION);
		}
		enum { NORMAL, BYTE, SHORT, LONG, LONG_LONG } length = NORMAL;
		if (*text == 'h') {
			length = SHORT;
			if (*++text == 'h') {
				length = BYTE;
				++text;
			}
		} else if (*text == 'l') {
			length = LONG;
			if (*++text == 'l') {
				length = LONG_LONG;
				++text;
			}
		}
		char conversion = *text;
		if (!conversion)
			fail("guest format has incomplete conversion");
		++text;
		int integer = conversion == 'd' || conversion == 'i' ||
			      conversion == 'u' || conversion == 'x' ||
			      conversion == 'X';
		int floating = conversion == 'f' || conversion == 'F' ||
			       conversion == 'g' || conversion == 'G' ||
			       conversion == 'e' || conversion == 'E';
		if ((!integer && !floating && conversion != 's' &&
		     conversion != 'c' && conversion != 'p') ||
		    (!integer && length != NORMAL &&
		     !(floating && length == LONG)))
			fail("unsupported guest format conversion");
		unsigned allowed = LEFT;
		if (floating || conversion == 'd' || conversion == 'i')
			allowed |= PLUS | SPACE | ZERO;
		if (floating || conversion == 'x' || conversion == 'X')
			allowed |= ALT | ZERO;
		if (conversion == 'u')
			allowed |= ZERO;
		if ((flags & ~allowed) ||
		    ((conversion == 'c' || conversion == 'p') &&
		     precision >= 0))
			fail("unsupported guest format flags or precision");

		/* Build a validated token with a host type matching the value below.
		 * Guest long is 32 bits even in a 64-bit component-test host. */
		char token[32];
		size_t count = 0;
		token[count++] = '%';
		if (flags & LEFT)
			token[count++] = '-';
		if (flags & PLUS)
			token[count++] = '+';
		if (flags & SPACE)
			token[count++] = ' ';
		if (flags & ALT)
			token[count++] = '#';
		if (flags & ZERO)
			token[count++] = '0';
		if (width)
			count += (size_t)snprintf(token + count,
						  sizeof(token) - count, "%u",
						  width);
		if (precision >= 0)
			count += (size_t)snprintf(token + count,
						  sizeof(token) - count, ".%d",
						  precision);
		if (integer && length == LONG_LONG) {
			token[count++] = 'l';
			token[count++] = 'l';
		}
		token[count++] = conversion;
		token[count] = 0;
		size_t available = sizeof(formatted) - used;
		int written;
		if (floating) {
			uint64_t bits = guest_format_argument(&argument, 8);
			double value;
			memcpy(&value, &bits, sizeof(value));
			written = snprintf(formatted + used, available, token,
					   value);
		} else if (integer && length == LONG_LONG) {
			uint64_t value = guest_format_argument(&argument, 8);
			if (conversion == 'd' || conversion == 'i')
				written = snprintf(formatted + used, available,
						   token,
						   (long long)(int64_t)value);
			else
				written = snprintf(formatted + used, available,
						   token,
						   (unsigned long long)value);
		} else {
			uint32_t value =
				(uint32_t)guest_format_argument(&argument, 4);
			if (conversion == 's') {
				const char *string =
					(const char *)(uintptr_t)value;
				(void)ascii_length(string);
				written = snprintf(formatted + used, available,
						   token, string);
			} else if (conversion == 'p')
				written = snprintf(formatted + used, available,
						   token,
						   (void *)(uintptr_t)value);
			else if (conversion == 'c')
				written = snprintf(formatted + used, available,
						   token,
						   (int)(unsigned char)value);
			else if (conversion == 'd' || conversion == 'i') {
				int32_t signed_value =
					length == BYTE	? (int8_t)value :
					length == SHORT ? (int16_t)value :
							  (int32_t)value;
				written = snprintf(formatted + used, available,
						   token, (int)signed_value);
			} else {
				unsigned int unsigned_value =
					length == BYTE	? (uint8_t)value :
					length == SHORT ? (uint16_t)value :
							  value;
				written = snprintf(formatted + used, available,
						   token, unsigned_value);
			}
		}
		if (written < 0 || (size_t)written >= available)
			fail("guest format output exceeds bound");
		used += (size_t)written;
	}
	formatted[used] = 0;
	if (!guest_data(out, used + 1, PF_W))
		fail("guest formatted output bounds");
	memcpy(out, formatted, used + 1);
	app.format_calls++;
	return (int)used;
}
