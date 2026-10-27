// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-check: package-embedded
 *
 * fplinux-phoneme MIDP native logging (core/log_base/include/midp_logging.h):
 * REPORT_* messages go to stderr as "fplinux-phoneme: midp[<level>/<channel>]:
 * <text>", so they stay out of the MIDlet's System.out on stdout. Which
 * levels are compiled in is REPORT_LEVEL of the generated
 * midp_constants_data.h.
 *
 * Also the PCSL print module (pcsl/print/pcsl_print.h), which only native
 * diagnostics use (e.g. the JPEG decoder's error messages): stderr as well.
 */
#include <stdarg.h>
#include <stdio.h>

#include <kni.h>
#include <midp_logging.h>
#include <pcsl_print.h>

void reportToLog(int severity, int channelID, char *message, ...)
{
	va_list ap;

	if (message == NULL)
		return;
	fprintf(stderr, "fplinux-phoneme: midp[%d/%d]: ", severity, channelID);
	va_start(ap, message);
	vfprintf(stderr, message, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void midp_logThreadId(char *message)
{
	(void)message;
}

void setupChannelsToLog(const char *pStrChannelList)
{
	(void)pStrChannelList;
}

void pcsl_print(const char *s)
{
	fputs(s, stderr);
}

void pcsl_print_chars(const char *s, int length)
{
	fwrite(s, 1, (size_t)length, stderr);
}
