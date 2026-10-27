// SPDX-License-Identifier: GPL-2.0-only
/*
 * fplinux-check: package-embedded
 *
 * fplinux-phoneme small system hooks of the MIDP core:
 * - native event queue lock (events/eventqueue_port/include): MIDP events
 *   are stored and read on the VM thread only, so no lock is needed
 *   (upstream stubs: events/eventqueue_port/stubs/native, which warn);
 * - the Configurator property callouts getLocalTimeZone (CLDC TimeZoneImpl)
 *   and getLocalHostName;
 * - the CLDC fallback socket protocol of classes.zip
 *   (com.sun.cldc.io.j2me.socket) gets natives that throw IOException; the
 *   MIDP GCF socket/http protocols use the PCSL port. These replace CLDC's
 *   BSDSocket.o, whose
 *   JVMSPI_CheckEvents() treats every blocked thread as a socket and would
 *   misread the MIDP threads blocked in midp_thread_wait().
 */
#include <stdio.h>
#include <time.h>

#include <kni.h>
#include <midpport_eventqueue.h>

void midp_createEventQueueLock(void)
{
}

void midp_destroyEventQueueLock(void)
{
}

void midp_waitAndLockEventQueue(void)
{
}

void midp_unlockEventQueue(void)
{
}

/* "GMT+05:00" from the process time zone (TZ, /etc/localtime). */
char *getLocalTimeZone(void)
{
	static char tz[16];
	time_t now = time(NULL);
	struct tm local;
	long off;

	if (localtime_r(&now, &local) == NULL)
		return "GMT";
	off = local.tm_gmtoff / 60;
	snprintf(tz, sizeof(tz), "GMT%c%02ld:%02ld", off < 0 ? '-' : '+',
		 (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60);
	return tz;
}

char *getLocalHostName(void)
{
	return "localhost";
}

/* ---- CLDC socket protocol (com.sun.cldc.io.j2me.socket.Protocol) ---- */

#define NO_NETWORK "use the MIDP socket protocol"

static void throw_no_network(void)
{
	KNI_ThrowNew("java/io/IOException", NO_NETWORK);
}

KNIEXPORT KNI_RETURNTYPE_INT KNIDECL(com_sun_cldc_io_j2me_socket_Protocol_open0)
{
	throw_no_network();
	KNI_ReturnInt(-1);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_cldc_io_j2me_socket_Protocol_readBuf)
{
	throw_no_network();
	KNI_ReturnInt(-1);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_cldc_io_j2me_socket_Protocol_readByte)
{
	throw_no_network();
	KNI_ReturnInt(-1);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_cldc_io_j2me_socket_Protocol_writeBuf)
{
	throw_no_network();
	KNI_ReturnInt(-1);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_cldc_io_j2me_socket_Protocol_writeByte)
{
	throw_no_network();
	KNI_ReturnInt(-1);
}

KNIEXPORT KNI_RETURNTYPE_INT
KNIDECL(com_sun_cldc_io_j2me_socket_Protocol_available0)
{
	KNI_ReturnInt(0);
}

KNIEXPORT KNI_RETURNTYPE_VOID
KNIDECL(com_sun_cldc_io_j2me_socket_Protocol_close0)
{
	KNI_ReturnVoid();
}
