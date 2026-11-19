// SPDX-License-Identifier: GPL-2.0-only
/* fplinux-check: package-embedded */
#include <kni.h>
#include <sni.h>
#include <zlib.h>

KNIEXPORT KNI_RETURNTYPE_INT KNIDECL(javax_microedition_m3g_Loader_checksum0)
{
	int offset = KNI_GetParameterAsInt(2);
	int length = KNI_GetParameterAsInt(3);
	jint checksum = 0;

	KNI_StartHandles(1);
	KNI_DeclareHandle(data);
	KNI_GetParameterAsObject(1, data);
	if (KNI_IsNullHandle(data) || offset < 0 || length < 0 ||
	    offset > KNI_GetArrayLength(data) - length) {
		KNI_ThrowNew("java/lang/IllegalArgumentException", "M3G data");
	} else {
		const Bytef *bytes = SNI_GetRawArrayPointer(data);

		checksum = (jint)adler32(1, bytes + offset, (uInt)length);
	}
	KNI_EndHandles();
	KNI_ReturnInt(checksum);
}

KNIEXPORT KNI_RETURNTYPE_BOOLEAN KNIDECL(javax_microedition_m3g_Loader_inflate0)
{
	int offset = KNI_GetParameterAsInt(2);
	int length = KNI_GetParameterAsInt(3);
	jboolean ok = KNI_FALSE;

	KNI_StartHandles(2);
	KNI_DeclareHandle(source);
	KNI_DeclareHandle(dest);
	KNI_GetParameterAsObject(1, source);
	KNI_GetParameterAsObject(4, dest);
	if (!KNI_IsNullHandle(source) && !KNI_IsNullHandle(dest) &&
	    KNI_GetArrayLength(dest) > 0 && offset >= 0 && length > 0 &&
	    offset <= KNI_GetArrayLength(source) - length) {
		const Bytef *bytes = SNI_GetRawArrayPointer(source);
		uLong in_len = (uLong)length;
		uLong expected = (uLong)KNI_GetArrayLength(dest);
		uLong out_len = expected;

		ok = uncompress2(SNI_GetRawArrayPointer(dest), &out_len,
				 bytes + offset, &in_len) == Z_OK &&
		     out_len == expected && in_len == (uLong)length;
	}
	KNI_EndHandles();
	KNI_ReturnBoolean(ok);
}
