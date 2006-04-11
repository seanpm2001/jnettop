/*
 *    jnettop, network online traffic visualiser
 *    Copyright (C) 2002-2005 Jakub Skopal
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *    $Header: /home/jakubs/DEV/jnettop-conversion/jnettop/jutil.h,v 1.6 2006-04-11 15:21:06 merunka Exp $
 *
 */

#ifndef __JUTIL_H__
#define __JUTIL_H__

#include "jbase.h"

const char * jutil_ValidateBPFFilter(char *filter);
int jutil_IsHostAggregation(int af, const jbase_mutableaddress *addr);
const char * jutil_Address2String(int af, const jbase_mutableaddress *src, char *dst, size_t cnt);
gboolean jutil_String2Address(const char *address, jbase_mutableaddress *dest, int *af);
guint jutil_ParseAggregation(const char *agg);
void jutil_formatNumber(guint32 n, gboolean onoffPackets, gchar *buf, int len);
void memand(char *buf1, const char *buf2, int length);

#endif
