/*
 *    jnettop, network online traffic visualiser
 *    Copyright (C) 2002 Jakub Skopal
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
 *    $Header: /home/jakubs/DEV/jnettop-conversion/jnettop/jnettop.c,v 1.25 2004-10-01 09:39:00 merunka Exp $
 *
 */

#include "jnettop.h"


/*
 * This stuff was copied out of Ethereal package by Gerald Combs <gerald@ethereal.com>
 * The point is, that we can use select() on platforms, where packet socket is
 * select()able. This prevents the capturer from taking all the processor time
 * doing g_tread_yeald() all the time.
 * Currently, this should happen only on BSD systems
 */
#if !defined(BSD)
# define USE_SELECT
#endif

gchar 	*NTOP_PROTOCOLS[] = { "UNK.", "IP", "TCP", "UDP", "ARP", "ETHER", 
                              "SLL", "AGGR.", "ICMP", "IP6", "TCP6", "UDP6", "ICMP6" };
gchar 	*NTOP_AGGREGATION[] = { "none", "port", "host" };

char		pcap_errbuf[PCAP_ERRBUF_SIZE];

int		devices_count;
ntop_device	*devices, *activeDevice, *newDevice;

FILE *		debugFile = NULL;

int		threadCount;

GQueue		*packetQueue;
GMutex		*packetQueueMutex;
GCond		*packetQueueCond;
GHashTable	*streamTable;
GMutex		*streamTableMutex;
GPtrArray	*streamArray;
GMutex		*streamArrayMutex;
GHashTable	*resolverCache;
GMutex		*resolverCacheMutex;
GTrashStack	*freePacketStack = NULL;
int		freePacketStackSize = 0;
GMutex		*freePacketStackMutex;
GMutex		*statusMutex;
char		*statusMessage;
GTimeVal	statusTimeout;

char		*configFileName;
GPtrArray	*bpfFilters;
char		*configDeviceName;

struct bpf_program	activeBPFFilter;
char		*activeBPFFilterName;
char		*newBPFFilter;
char		*newBPFFilterName;

char		*commandLineRule;

GThread		*snifferThread;
GThread		*sorterThread;
GThread		*processorThread;
GThread		*displayThread;
GThreadPool	*resolverThreadPool;

GTimeVal	startTime;
GTimeVal	historyTime;

guint32		totalSrcBytes, totalDstBytes, totalBytes;
guint32		totalSrcPackets, totalDstPackets, totalPackets;
guint32		totalSrcBPS, totalDstBPS, totalBPS;
guint32		totalSrcPPS, totalDstPPS, totalPPS;

GMutex		*displayStreamsMutex;
ntop_stream	**displayStreams;
int		displayStreamsCount;
gchar 		line0FormatString[512], line1FormatString[512], line2FormatString[512];

gboolean	onoffContentFiltering;
gboolean	onoffBitValues;
gboolean	onoffPackets;
gboolean	onoffSuspended;
gboolean	onoffPromisc;

#define		DISPLAYMODE_NORMAL		0
#define		DISPLAYMODE_BPFFILTERS		1
#define		DISPLAYMODE_HELP		2

int		displayMode = DISPLAYMODE_NORMAL;

WINDOW		*listWindow;

guint		localAggregation;
guint		remoteAggregation;

const char * validateBPFFilter(char *filter) {
	const char *ret = NULL;
	struct bpf_program program;
	pcap_t *pcap;
	pcap = pcap_open_dead(DLT_EN10MB, 1500);
	if (pcap_compile(pcap, &program, filter, 0, 0xFFFFFFFF) == -1) {
		ret = pcap_geterr(pcap);
	} else {
		pcap_freecode(&program);
	}
	pcap_close(pcap);
	return ret;
}

void	setToHostAggregation(int af, ntop_mutableaddress *addr) {
	switch (af) {
		case AF_INET:
			addr->addr4.s_addr = htonl(0x01000000);
			break;
		case AF_INET6:
			addr->addr6.s6_addr32[0] = 0x0;
			addr->addr6.s6_addr32[1] = 0x0;
			addr->addr6.s6_addr32[2] = 0x0;
			addr->addr6.s6_addr32[3] = htonl(0x01000000);
			break;
	}
}

int	isHostAggregation(int af, const ntop_mutableaddress *addr) {
	switch (af) {
		case AF_INET:
			return addr->addr4.s_addr == htonl(0x01000000);
		case AF_INET6:
			return addr->addr6.s6_addr32[0] == 0x0 && addr->addr6.s6_addr32[1] == 0x0 && addr->addr6.s6_addr32[2] == 0x0  && addr->addr6.s6_addr32[3] == htonl(0x01000000);
	}
	return 0;
}

const char * address2String(int af, const ntop_mutableaddress *src, char *dst, size_t cnt) {
	if (isHostAggregation(af, src)) {
		*dst = '\0';
		return dst;
	}
#if HAVE_INET_NTOP
	return inet_ntop(af, (const void *)src, dst, cnt);
#elif HAVE_INET_NTOA
	static GStaticMutex mutex = G_STATIC_MUTEX_INIT;
	char *tmp, *ret = NULL;
	g_static_mutex_lock(&mutex);
	switch (af) {
	case AF_INET:
		tmp = inet_ntoa(src->addr4);
		break;
	case AF_INET6:
		g_snprintf(dst, cnt, "ipv6-res.-n/a"); //TODO: find an alternative way to resolve IPv6
		return dst;
	}
	if (tmp && strlen(tmp)<cnt-1) {
		strcpy(dst, tmp);
		ret = dst;
	}
	g_static_mutex_unlock(&mutex);
	return ret;
#else
# error "no funtion to convert internet address to string found by configure"
#endif
}

void debug(const char *format, ...) {
	if (debugFile) {
		va_list ap;
		va_start(ap, format);
		vfprintf(debugFile, format, ap);
		va_end(ap);
	}
}

void freeStream(gpointer ptr) {
	ntop_stream *s = (ntop_stream *)ptr;
	if (s->filterDataFreeFunc)
		s->filterDataFreeFunc(s);
	g_free(s);
}

void createDevice(char *deviceName) {
	devices_count = 1;
	devices = g_new(ntop_device, 1);
	devices[0].name = g_strndup((gchar*)deviceName, strlen(deviceName));
}

void lookupDevices() {
#if HAVE_PCAP_FINDALLDEVS
	pcap_if_t	*head, *t;
	int		i;
	if (pcap_findalldevs(&head, pcap_errbuf) != 0) {
		fprintf(stderr, "pcap_findalldevs: %s\n", pcap_errbuf);
		exit(255);
	}
	devices_count = 0;
	t = head;
	while (t) {
		devices_count ++;
		t = t->next;
	}
	devices = g_new0(ntop_device, devices_count);
	t = head;
	i = 0;
	while (t) {
		devices[i++].name = g_strndup((gchar*)t->name, strlen(t->name));
		t = t->next;
	}
	pcap_freealldevs(head);
#else
	char		*name;
	devices_count = 1;
	devices = g_new(ntop_device, 1);
	name = pcap_lookupdev(pcap_errbuf);
	if (!name) {
		fprintf(stderr, "pcap_lookupdev: %s\n", pcap_errbuf);
		exit(255);
	}
	devices[0].name = g_strndup((gchar*)name, strlen(name));
#endif
}

void checkDevices() {
	struct ifreq ifr;
	int s,i;

	memset(&ifr, 0, sizeof(struct ifreq));
	s = socket(PF_INET, SOCK_DGRAM, 0);
	if (s==-1) {
		fprintf(stderr, "Could not open datagram socket used to discover HW addresses of interfaces: %s\n", strerror(errno));
		exit(1);
	}
	for (i=0; i<devices_count; i++) {
		strncpy(ifr.ifr_name, devices[i].name, IFNAMSIZ);
		ifr.ifr_hwaddr.sa_family = AF_UNSPEC;
#ifdef SIOCGIFHWADDR
		if (ioctl(s, SIOCGIFHWADDR, &ifr) == -1) {
#else
		if (ioctl(s, SIOCGIFADDR, &ifr) == -1) {
#endif
			fprintf(stderr, "Could not get HW address of interface %s: %s\n", devices[i].name, strerror(errno));
		} else {
			memcpy(&devices[i].hwaddr, &ifr.ifr_hwaddr, sizeof(struct sockaddr));
		}
	}
	close(s);
}
	
guint hashStream(gconstpointer key) {
	const ntop_stream	*stream = (const ntop_stream *)key;
	guint hash = 0;
	hash = stream->src.addr6.s6_addr32[0];
	hash ^= stream->src.addr6.s6_addr32[1];
	hash ^= stream->src.addr6.s6_addr32[2];
	hash ^= stream->src.addr6.s6_addr32[3];
	hash ^= stream->dst.addr6.s6_addr32[0];
	hash ^= stream->dst.addr6.s6_addr32[1];
	hash ^= stream->dst.addr6.s6_addr32[2];
	hash ^= stream->dst.addr6.s6_addr32[3];
	hash ^= (((guint)stream->srcport) << 16) + (guint)stream->dstport;
	return hash;
}

gboolean compareStream(gconstpointer a, gconstpointer b) {
	const ntop_stream *astr = (const ntop_stream *)a;
	const ntop_stream *bstr = (const ntop_stream *)b;
	if (astr->proto == bstr->proto &&
			astr->srcport == bstr->srcport &&
			astr->dstport == bstr->dstport &&
			IN6_ARE_ADDR_EQUAL(&astr->src.addr6, &bstr->src.addr6) &&
			IN6_ARE_ADDR_EQUAL(&astr->dst.addr6, &bstr->dst.addr6)
			)
		return TRUE;
	return FALSE;
}

gint compareStreamByStat(gconstpointer a, gconstpointer b) {
	const ntop_stream	*astr = *(const ntop_stream **)a;
	const ntop_stream	*bstr = *(const ntop_stream **)b;
	if (onoffPackets) {
		if (astr->totalpps > bstr->totalpps)
			return -1;
		else if (astr->totalpps == bstr->totalpps)
			return 0;
	} else {
		if (astr->totalbps > bstr->totalbps)
			return -1;
		else if (astr->totalbps == bstr->totalbps)
			return 0;
	}
	return 1;
}

guint hashResolvEntry(gconstpointer key) {
	const ntop_resolv_entry *resolv = key;
	guint hash = 0;
	hash = resolv->addr.addr6.s6_addr32[0];
	hash ^= resolv->addr.addr6.s6_addr32[1];
	hash ^= resolv->addr.addr6.s6_addr32[2];
	hash ^= resolv->addr.addr6.s6_addr32[3];
	return hash;
}

gboolean compareResolvEntry(gconstpointer a, gconstpointer b) {
	ntop_resolv_entry	*aa, *bb;
	if(a == b) return 1;
	aa = (ntop_resolv_entry *) a;
	bb = (ntop_resolv_entry *) b;
	if(aa->af != bb->af)
		return 0;
	return !memcmp(&aa->addr, &bb->addr, sizeof(aa->addr));
}

void	aggregateStream(ntop_stream *stream) {
	switch (localAggregation) {
		case AGG_HOST:
			setToHostAggregation(NTOP_AF(stream->proto), &stream->src);
		case AGG_PORT:
			stream->srcport = -1;
	}
	switch (remoteAggregation) {
		case AGG_HOST:
			setToHostAggregation(NTOP_AF(stream->proto), &stream->dst);
		case AGG_PORT:
			stream->dstport = -1;
	}
}

void	sortPacket(const ntop_packet *packet) {
	ntop_stream	packetStream;
	ntop_stream	*stat;
	ntop_resolv_entry	key;
	ntop_payload_info	payloadInfo[NTOP_PROTO_MAX];
	totalBytes += packet->header.len;
	totalPackets ++;
	memset(&packetStream, 0, sizeof(ntop_stream));
	resolveStream(packet, &packetStream, payloadInfo);
	aggregateStream(&packetStream);
	g_mutex_lock(streamTableMutex);
	stat = (ntop_stream *)g_hash_table_lookup(streamTable, &packetStream);
	if (stat == NULL) {
		ntop_resolv_entry *rentry;
		stat = g_new0(ntop_stream, 1);
		memcpy(stat, &packetStream, sizeof(ntop_stream));
		g_get_current_time(&stat->firstSeen);
		g_hash_table_insert(streamTable, stat, stat);
		g_mutex_unlock(streamTableMutex);

		if (onoffContentFiltering)
			assignDataFilter(stat);
		
		memcpy(&key.addr, &packetStream.src, sizeof(key.addr));
		key.name = NULL;
		key.af = NTOP_AF(packetStream.proto);
		g_mutex_lock(resolverCacheMutex);
		rentry = g_hash_table_lookup(resolverCache, &key);
		if (rentry == NULL) {
			rentry = g_new0(ntop_resolv_entry, 1);
			memcpy(rentry, &key, sizeof(key));
			g_hash_table_insert(resolverCache, rentry, rentry);
			g_mutex_unlock(resolverCacheMutex);
			g_thread_pool_push(resolverThreadPool, rentry, NULL);
			g_mutex_lock(resolverCacheMutex);
		}
		stat->srcresolv = rentry;
		memcpy(&key.addr, &packetStream.dst, sizeof(key.addr));
		rentry = g_hash_table_lookup(resolverCache, &key);
		if (rentry == NULL) {
			rentry = g_new0(ntop_resolv_entry, 1);
			memcpy(rentry, &key, sizeof(key));
			g_hash_table_insert(resolverCache, rentry, rentry);
			g_mutex_unlock(resolverCacheMutex);
			g_thread_pool_push(resolverThreadPool, rentry, NULL);
		} else {
			g_mutex_unlock(resolverCacheMutex);
		}
		stat->dstresolv = rentry;

		g_mutex_lock(streamArrayMutex);
		g_ptr_array_add(streamArray, stat);
		g_mutex_unlock(streamArrayMutex);
	} else {
		g_mutex_unlock(streamTableMutex);
	}
	if (packetStream.direction) {
		stat->dstbytes += packet->header.len;
		stat->dstpackets ++;
		*stat->hdstbytes += packet->header.len;
		stat->hdstpackets[0]++;
		stat->hdstbytessum += packet->header.len;
		stat->hdstpacketssum++;
		totalDstBytes += packet->header.len;
		totalDstPackets++;
	} else {
		stat->srcbytes += packet->header.len;
		stat->srcpackets ++;
		*stat->hsrcbytes += packet->header.len;
		stat->hsrcpackets[0]++;
		stat->hsrcbytessum += packet->header.len;
		stat->hsrcpacketssum++;
		totalSrcBytes += packet->header.len;
		totalSrcPackets++;
	}
	stat->totalbytes += packet->header.len;
	stat->totalpackets ++;
	g_get_current_time(&stat->lastSeen);

	if (onoffContentFiltering && stat->filterDataFunc) {
		stat->filterDataFunc(stat, packet, packetStream.direction, payloadInfo);
	}
}

void	markAllAsDead() {
	int i;
	g_mutex_lock(streamArrayMutex);
	for (i=0; i<streamArray->len; i++) {
		ntop_stream *s = (ntop_stream *)g_ptr_array_index(streamArray, i);
		s->dead=6;
	}
	g_mutex_unlock(streamArrayMutex);
}

void	updateBPS() {
	GTimeVal	currentDateTime;
	uint		i;
	guint32		srcbps = 0;
	guint32		dstbps = 0;
	guint32		srcpps = 0;
	guint32		dstpps = 0;

	g_get_current_time(&currentDateTime);
	currentDateTime.tv_sec ++;

	for (i=0; i<streamArray->len; i++) {
		ntop_stream *s = (ntop_stream *)g_ptr_array_index(streamArray, i);
		int byteswindow = ( currentDateTime.tv_sec - s->firstSeen.tv_sec );
		if (byteswindow > HISTORY_LENGTH)
			byteswindow = HISTORY_LENGTH;
			
		srcbps += (s->srcbps = s->hsrcbytessum / byteswindow);
		s->hsrcbytessum -= s->hsrcbytes[HISTORY_LENGTH-1];
		memmove(s->hsrcbytes+1, s->hsrcbytes, sizeof(guint)*(HISTORY_LENGTH-1));
		s->hsrcbytes[0] = 0;
		dstbps += (s->dstbps = s->hdstbytessum / byteswindow);
		s->hdstbytessum -= s->hdstbytes[HISTORY_LENGTH-1];
		memmove(s->hdstbytes+1, s->hdstbytes, sizeof(guint)*(HISTORY_LENGTH-1));
		s->hdstbytes[0] = 0;
		s->totalbps = s->srcbps + s->dstbps;
		
		srcpps += (s->srcpps = s->hsrcpacketssum / byteswindow);
		s->hsrcpacketssum -= s->hsrcpackets[HISTORY_LENGTH-1];
		memmove(s->hsrcpackets+1, s->hsrcpackets, sizeof(guint)*(HISTORY_LENGTH-1));
		s->hsrcpackets[0] = 0;
		dstpps += (s->dstpps = s->hdstpacketssum / byteswindow);
		s->hdstpacketssum -= s->hdstpackets[HISTORY_LENGTH-1];
		memmove(s->hdstpackets+1, s->hdstpackets, sizeof(guint)*(HISTORY_LENGTH-1));
		s->hdstpackets[0] = 0;
		s->totalpps = s->srcpps + s->dstpps;
		
		if (!s->dead && currentDateTime.tv_sec - s->lastSeen.tv_sec > 10) {
			s->dead ++;
		}
	}

	totalSrcBPS = srcbps;
	totalDstBPS = dstbps;
	totalBPS = srcbps + dstbps;
	totalSrcPPS = srcpps;
	totalDstPPS = dstpps;
	totalPPS = srcpps + dstpps;
}

int	activeLines=1, activeColumns=1;

void formatNumber(guint32 n, gchar *buf, int len) {
	gchar suffixes[] = {'b','k','m','g','t'};
	gchar fmt[64];
	int  mag = 0;
	int  ipart,fpart = 0;
	gdouble f = (gdouble)n;
	while (mag<4 && f>1000.0) {
		mag ++;
		f /= 1024.0;
	}
	sprintf(fmt, "%.0f", f);
	ipart = strlen(fmt);
	while (ipart+1+fpart+2 < len && mag > 0)
		fpart ++;
	if (ipart+1+fpart+2 > len) {
		sprintf(buf, "ERR");
		return;
	}
	sprintf(fmt, "%%%d.%df%c", ipart, fpart, !mag && onoffPackets ? 'p' : suffixes[mag]);
	sprintf(buf, fmt, f);
}

void drawStatus(guchar *msg) {
	g_mutex_lock(statusMutex);
	statusMessage = g_strdup(msg);
	g_get_current_time(&statusTimeout);
	g_time_val_add(&statusTimeout, 1000000);
	g_mutex_unlock(statusMutex);
	attron(A_BOLD);
	mvprintw(2, 0, "%s", statusMessage);
	clrtoeol();
	attroff(A_BOLD);
	refresh();
}

void drawScreen() {
	if (LINES != activeLines || COLS != activeColumns || !activeLines || !activeColumns) {
		activeLines = LINES;
		activeColumns = COLS;

		if (activeLines < 20 || activeColumns < 80) {
			endwin();
			fprintf(stderr, "Too small terminal (detected size: %dx%d), minimum required size: 80x20\n", activeColumns, activeLines);
			exit(255);
		}

		attrset(A_NORMAL);

		mvprintw(0, 0, "run XXX:XX:XX device XXXXXXXXXX pkt[f]ilter: XXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
		mvprintw(1, 0, "[c]ntfilter: XXX [b]ps=XXXXXXX [l]ocal aggr.: XXXX [r]emote aggr.: XXXX   ");
#if HAVE_PCAP_FINDALLDEVS
		if (devices_count>1) {
			mvprintw(2, 10, "[0]-[9] switch device");
		}
#endif
		mvprintw(0, activeColumns-1, ".");

		{
			int addrColumns = (activeColumns - 48) / 2;
			sprintf(line0FormatString, "%%-%d.%ds %%7.7s %%7.7s %%8.8s", activeColumns-25, activeColumns-25);
			sprintf(line1FormatString, " %%-%d.%ds %%5.5s %%6.6s  %%-%d.%ds %%5.5s  %%7.7s %%7.7s %%8.8s", addrColumns, addrColumns, addrColumns, addrColumns);
			sprintf(line2FormatString, "  %%-%d.%ds", activeColumns-3, activeColumns-3);
		}

		if (listWindow) {
			delwin(listWindow);
		}
		listWindow = newwin(activeLines-8, activeColumns, 5, 0);
	}
	g_mutex_lock(statusMutex);
	if (statusMessage == NULL) {
		mvprintw(2, 0, "[q]uit [h]elp [s]orting [p]ackets");
	} else {
		GTimeVal tv;
		attron(A_BOLD);
		mvprintw(2, 0, statusMessage);
		attroff(A_BOLD);
		g_get_current_time(&tv);
		if (tv.tv_sec >= statusTimeout.tv_sec) {
			g_free(statusMessage);
			statusMessage = NULL;
		}
	}
	g_mutex_unlock(statusMutex);
	clrtoeol();
}

void drawHeader() {
	GTimeVal	currentTime;
	gchar		timeBuffer[32];
	gchar srcbps[10], dstbps[10], bps[10], total[10], totalsrc[10], totaldst[10];
	int i;
	struct tm tm;

	attron(A_BOLD);
	
	g_get_current_time(&currentTime);
	localtime_r(&currentTime.tv_sec, &tm);
	sprintf(timeBuffer, "%3d:%02d:%02d", (int)((currentTime.tv_sec-startTime.tv_sec)/3600), (int)((currentTime.tv_sec-startTime.tv_sec)%3600/60), (int)((currentTime.tv_sec-startTime.tv_sec)%60));
	mvprintw(0, 4, "%s", timeBuffer);
	if (activeDevice)
		mvprintw(0, 21, "%-10s", activeDevice->name);
	mvprintw(0, 45, "%-29.29s", activeBPFFilterName?activeBPFFilterName:"none");
	mvprintw(1, 13, "%s", onoffContentFiltering?"on ":"off");
	mvprintw(1, 23, "%s", onoffPackets ? "pckts/s" : (onoffBitValues?"bits/s ":"bytes/s"));
	mvprintw(1, 46, "%s", NTOP_AGGREGATION[localAggregation]);
	mvprintw(1, 67, "%s", NTOP_AGGREGATION[remoteAggregation]);

	attroff(A_BOLD);

	formatNumber(onoffPackets?totalPPS:(onoffBitValues?8:1)*totalBPS, bps, 6);
	g_strlcat(bps, "/s", sizeof(bps));
	formatNumber(onoffPackets?totalSrcPPS:(onoffBitValues?8:1)*totalSrcBPS, srcbps, 6);
	g_strlcat(srcbps, "/s", sizeof(srcbps));
	formatNumber(onoffPackets?totalDstPPS:(onoffBitValues?8:1)*totalDstBPS, dstbps, 6);
	g_strlcat(dstbps, "/s", sizeof(dstbps));
	mvprintw(activeLines-2, 0, line0FormatString, "TOTAL", srcbps, dstbps, bps);

	formatNumber(onoffPackets?totalPackets:(onoffBitValues?8:1)*totalBytes, total, 6);
	formatNumber(onoffPackets?totalSrcPackets:(onoffBitValues?8:1)*totalSrcBytes, totalsrc, 6);
	formatNumber(onoffPackets?totalDstPackets:(onoffBitValues?8:1)*totalDstBytes, totaldst, 6);
	mvprintw(activeLines-1, 0, line1FormatString, "", "", "", "", "", totalsrc, totaldst, total);

	mvchgat(activeLines-2, 0, activeColumns-25, A_BOLD, 0, NULL);

	for (i=0; i<activeColumns; i++)
		mvaddch(activeLines-3, i, ACS_HLINE);

	attron(A_REVERSE);

	mvprintw(3, 0, line0FormatString, "LOCAL <-> REMOTE", onoffPackets ? "TXPPS" : "TXBPS",
		onoffPackets ? "RXPPS" : "RXBPS", onoffPackets ? "TOTALPPS" : "TOTALBPS");
	mvprintw(4, 0, line1FormatString, "(IP)", "PORT", "PROTO", "(IP)", "PORT", "TX", "RX", "TOTAL");

	attroff(A_REVERSE);
}

void resolverThreadFunc(gpointer task, gpointer user_data) {
	ntop_resolv_entry *entry = (ntop_resolv_entry *)task;
	gchar buffer[4096];
	struct hostent shentry, *hentry;
	int  e, ret, size;
	gchar *name;

	ret = 0;
	size = entry->af == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr);
#if HAVE_GETHOSTBYADDR_R_8
	ret = gethostbyaddr_r(&entry->addr, size, entry->af, &shentry, buffer, 4096, &hentry, &e);
#elif HAVE_GETHOSTBYADDR_R_7
	hentry = gethostbyaddr_r(&entry->addr, size, entry->af, &shentry, buffer, 4096, &e);
#else
# error "No suitable gethostbyaddr_r found by configure"
#endif
	if (ret || e) {
		return;
	}
	name = g_strdup(hentry->h_name);
	entry->name = name;
}

gpointer sorterThreadFunc(gpointer data) {
	threadCount ++;

	while (activeDevice != NULL) {
		guint		i, j;
		int		lines,oldLines;
		ntop_stream	**streams,**oldStreams;
		GTimeVal	t;

		lines = (activeLines - 8) / 3;

		streams = g_new0(ntop_stream *, lines);
		
		g_mutex_lock(streamArrayMutex);
		if (streamArray->len > 0) {
			updateBPS();
			if (!onoffSuspended)
				g_ptr_array_sort(streamArray, (GCompareFunc)compareStreamByStat);
		}
		for (i=0,j=0; i<streamArray->len && j<lines; i++) {
			ntop_stream *s = (ntop_stream *)g_ptr_array_index(streamArray, i);
			if (s->dead > 5) {
				continue;
			}
			s->displayed ++;
			streams[j++] = s;
		}
		lines = j;
		g_mutex_unlock(streamArrayMutex);

		g_mutex_lock(displayStreamsMutex);
		oldStreams = displayStreams;
		oldLines   = displayStreamsCount;
		displayStreams = streams;
		displayStreamsCount = lines;
		g_mutex_unlock(displayStreamsMutex);

		for (i=0; i<oldLines; i++) {
			oldStreams[i]->displayed --;
		}
		if (oldStreams)
			g_free(oldStreams);

		g_mutex_lock(streamArrayMutex);
		g_mutex_lock(streamTableMutex);

		for (i=0; i<streamArray->len; i++) {
			ntop_stream *s = (ntop_stream *)g_ptr_array_index(streamArray, i);
			if (s->dead && ++s->dead > 7 && !s->displayed) {
				g_ptr_array_remove_index_fast ( streamArray, i );
				g_hash_table_remove ( streamTable, s );
				freeStream(s);
				i--;
			}
		}

		g_mutex_unlock(streamTableMutex);
		g_mutex_unlock(streamArrayMutex);

		g_get_current_time(&t);
		g_usleep(1000000 - t.tv_usec);
	}

	threadCount --;
	return NULL;
}

gboolean	removeStreamTableEntry(gpointer key, gpointer value, gpointer user_data) {
	freeStream(key);
	// value is the same pointer as key
	return TRUE;
}

void     clearStatistics() {
	gpointer	ptr;
	int            	i;

	while ((ptr = g_queue_pop_tail(packetQueue))) {
		g_free(ptr);
	}
	for (i=streamArray->len-1; i>=0; i--) {
		g_ptr_array_remove_index_fast(streamArray, i);
	}
	g_hash_table_foreach_remove(streamTable, (GHRFunc)removeStreamTableEntry, NULL);
}

void	doDisplayStreams() {
	int i;
	for (i=0; i<displayStreamsCount; i++) {
		gchar srcaddr[INET6_ADDRSTRLEN + 1], dstaddr[INET6_ADDRSTRLEN + 1];
		gchar srcport[10], dstport[10], srcbps[10], dstbps[10], bps[10];
		gchar total[10], totalsrc[10], totaldst[10];
		uint tmp;
		gchar linebuffer[1024];
		gchar *psrcaddr, *pdstaddr;
		ntop_stream *s = displayStreams[i];
		tmp = onoffPackets ? s->totalpps : (onoffBitValues?8:1)*s->totalbps;
		formatNumber(tmp, bps, 6);
		g_strlcat(bps, "/s", sizeof(bps));
		tmp = onoffPackets ? s->srcpps : (onoffBitValues?8:1)*s->srcbps;
		formatNumber(tmp, srcbps, 6);
		g_strlcat(srcbps, "/s", sizeof(srcbps));
		tmp = onoffPackets ? s->dstpps : (onoffBitValues?8:1)*s->dstbps;
		formatNumber(tmp, dstbps, 6);
		g_strlcat(dstbps, "/s", sizeof(dstbps));
		formatNumber(onoffPackets ? s->totalpackets : s->totalbytes, total, 6);
		formatNumber(onoffPackets ? s->srcpackets : s->srcbytes, totalsrc, 6);
		formatNumber(onoffPackets ? s->dstpackets : s->dstbytes, totaldst, 6);
		address2String(NTOP_AF(s->proto), &s->src, srcaddr, INET6_ADDRSTRLEN);
		if (s->srcresolv == NULL || s->srcresolv->name == NULL) {
			psrcaddr = srcaddr;
		} else {
			psrcaddr = s->srcresolv->name;
		}
		address2String(NTOP_AF(s->proto), &s->dst, dstaddr, INET6_ADDRSTRLEN);
		if (s->dstresolv == NULL || s->dstresolv->name == NULL) {
			pdstaddr = dstaddr;
		} else {
			pdstaddr = s->dstresolv->name;
		}
		if (s->srcport == -1)
			strcpy(srcport, "AGGR.");
		else
			sprintf(srcport, "%d", s->srcport);
		if (s->dstport == -1)
			strcpy(dstport, "AGGR.");
		else
			sprintf(dstport, "%d", s->dstport);
		sprintf(linebuffer, "%s <-> %s", psrcaddr, pdstaddr);
		mvwprintw(listWindow, i*3, 0, line0FormatString, linebuffer, srcbps, dstbps, bps);
		mvwchgat(listWindow, i*3, 0, activeColumns-25, A_BOLD, 0, NULL);
		mvwprintw(listWindow, i*3+1, 0, line1FormatString, srcaddr, srcport, NTOP_PROTOCOLS[s->proto], dstaddr, dstport, totalsrc, totaldst, total);
		mvwprintw(listWindow, i*3+2, 0, line2FormatString, s->filterDataString);
	}
}

void    doDisplayWholeScreen() {
	g_mutex_lock(displayStreamsMutex);
	drawScreen();
	drawHeader();
	werase(listWindow);
}

gpointer displayThreadFunc(gpointer data) {
	threadCount ++;
	g_usleep(500000);

	while (activeDevice != NULL) {
		int i;
		
		doDisplayWholeScreen();

		switch (displayMode) {
		case DISPLAYMODE_NORMAL:
			doDisplayStreams();
			break;
		case DISPLAYMODE_BPFFILTERS:
			wattron(listWindow, A_BOLD);
			mvwprintw(listWindow, 1, 0, "Select rule you want to apply:");
			wattroff(listWindow, A_BOLD);
			mvwprintw(listWindow, 3, 5, "[.] None");
			if (commandLineRule) {
				mvwprintw(listWindow, 4, 5, "[,] %s", commandLineRule);
			}
			for (i=0; i<bpfFilters->len/2; i++) {
				mvwprintw(listWindow, i+5, 5, "[%c] %s", 'a'+i, g_ptr_array_index(bpfFilters, i*2));
			}
			if (bpfFilters->len == 0) {
				mvwprintw(listWindow, 6, 5, "You have no predefined filter rules. See README file for explanation");
				mvwprintw(listWindow, 7, 5, "on how to predefine filter rules");
			}
			break;
		case DISPLAYMODE_HELP:
			mvwprintw(listWindow, 2, 0, "I must write something here... :)");
			mvwprintw(listWindow, 4, 0, "Press any key to return.");
			break;
		}

		g_mutex_unlock(displayStreamsMutex);

		wnoutrefresh(listWindow);
		refresh();

		i = getch();
		if (i==ERR) {
			g_usleep(1000000);
		} else {
			switch (displayMode) {
			case DISPLAYMODE_NORMAL:
				switch (i) {
					case 'q':
					case 'Q':
						drawStatus("Please wait, shutting down...");
						activeDevice = NULL;
						break;
					case 'c':
						onoffContentFiltering = !onoffContentFiltering;
						break;
					case 'b':
						onoffBitValues = !onoffBitValues;
						break;
					case 'p':
						onoffPackets = !onoffPackets;
						break;
					case 's':
						onoffSuspended = !onoffSuspended;
						if (onoffSuspended)
							drawStatus("Streams sorting suspended.");
						else
							drawStatus("Streams sorting resumed.");
						break;
					case 'f':
						displayMode = DISPLAYMODE_BPFFILTERS;
						break;
					case 'h':
						displayMode = DISPLAYMODE_HELP;
						break;
					case 'l':
						markAllAsDead();
						localAggregation = (localAggregation + 1) % 3;
						break;
					case 'r':
						markAllAsDead();
						remoteAggregation = (remoteAggregation + 1) % 3;
						break;
					case '0':
					case '1':
					case '2':
					case '3':
					case '4':
					case '5':
					case '6':
					case '7':
					case '8':
					case '9':
						i -= '0';
						if (devices_count>1 && devices_count > i) {
							drawStatus("Please wait, cleaning up...");
							newDevice = devices + i;
							activeDevice = NULL;
						}
						break;
				}
				break;
			case DISPLAYMODE_BPFFILTERS:
				if ((i == '.') || (commandLineRule && (i == ',')) || ((i >= 'a') && (i < 'a' + (bpfFilters->len/2)))) {
					drawStatus("Please wait, cleaning up...");
					switch (i) {
					case '.':
						newBPFFilter = NULL;
						newBPFFilterName = NULL;
						break;
					case ',':
						newBPFFilterName = commandLineRule;
						newBPFFilter = commandLineRule;
						break;
					default:
						newBPFFilter = g_ptr_array_index(bpfFilters, (i - 'a')*2 + 1);
						newBPFFilterName = (char *) g_ptr_array_index(bpfFilters, (i - 'a')*2 );
						break;
					}
					newDevice = activeDevice;
					activeDevice = NULL;
					displayMode = DISPLAYMODE_NORMAL;
					break;
				}
				break;
			case DISPLAYMODE_HELP:
				displayMode = DISPLAYMODE_NORMAL;
				break;
			}
		}
	}

	threadCount --;
	return NULL;
}

ntop_packet *allocNtopPacket() {
	ntop_packet *ptr;
	g_mutex_lock(freePacketStackMutex);
	ptr = (ntop_packet *)g_trash_stack_pop(&freePacketStack);
	if (ptr) {
		freePacketStackSize --;
	}
	g_mutex_unlock(freePacketStackMutex);
	if (!ptr) {
		ptr = g_new(ntop_packet, 1);
	}
	return ptr;
}

void freeNtopPacket(ntop_packet *packet) {
	g_mutex_lock(freePacketStackMutex);
	if (freePacketStackSize < FREEPACKETSTACK_PEEK) {
		g_trash_stack_push(&freePacketStack, packet);
		freePacketStackSize ++;
		packet = NULL;
	}
	g_mutex_unlock(freePacketStackMutex);
	if (packet)
		g_free(packet);
	
}

gpointer processorThreadFunc(gpointer data) {
	threadCount ++;
	g_mutex_lock(packetQueueMutex);
	while (activeDevice != NULL) {
		ntop_packet	*packet;
		packet = (ntop_packet *)g_queue_pop_tail(packetQueue);
		if (packet == NULL) {
			g_cond_wait(packetQueueCond, packetQueueMutex);
			continue;
		}

		g_mutex_unlock(packetQueueMutex);

		sortPacket(packet);
		freeNtopPacket(packet);

		g_mutex_lock(packetQueueMutex);
	}
	g_mutex_unlock(packetQueueMutex);
	threadCount --;

	return NULL;
}

gboolean	packetReceived;
int		deviceDataLink;

void     dispatch_callback(const u_char *udata, const struct pcap_pkthdr *hdr, const guchar *pcappacket) {
	ntop_packet * packet;
	packet = allocNtopPacket();
	packet->device = activeDevice;
	packet->dataLink = deviceDataLink;
	memcpy(&(packet->header), hdr, sizeof(struct pcap_pkthdr));
	if (packet->header.caplen > BUFSIZ)
		packet->header.caplen = BUFSIZ;
	memcpy(packet->data, pcappacket, packet->header.caplen);
	g_mutex_lock(packetQueueMutex);
	g_queue_push_head(packetQueue, packet);
	g_mutex_unlock(packetQueueMutex);
	g_cond_signal(packetQueueCond);
}

gpointer snifferThreadFunc(gpointer data) {
	pcap_t		*handle = NULL;
	ntop_device	*device = NULL;
	gchar		pcap_errbuf[PCAP_ERRBUF_SIZE];
	gboolean	isFilterUsed = FALSE;

	threadCount ++;

	while (1) {
		if (device != activeDevice) {
			if (device) {
				pcap_close(handle);
			}
			if (isFilterUsed) {
				pcap_freecode(&activeBPFFilter);
			}
			device = activeDevice;
			if (!device) {
				g_cond_signal(packetQueueCond);
				threadCount --;

				return NULL;
			}
			handle = pcap_open_live((char*)device->name, BUFSIZ, onoffPromisc, 10, pcap_errbuf);
			if (handle == NULL) {
				char BUF[PCAP_ERRBUF_SIZE + 128];
				snprintf(BUF, PCAP_ERRBUF_SIZE + 128, "Not sniffing. Error while initializing %s: %s", device->name, pcap_errbuf);
				drawStatus(BUF);
				break;
			}
#if HAVE_PCAP_SETNONBLOCK
			pcap_setnonblock(handle, 1, NULL);
#endif
			activeBPFFilterName = NULL;
			if (newBPFFilter) {
				isFilterUsed = FALSE;
				debug("Filter: %s\n", newBPFFilter);
				if (pcap_compile(handle, &activeBPFFilter, newBPFFilter, 0, 0xFFFFFFFF) == -1) {
					char BUF[PCAP_ERRBUF_SIZE + 128];
					snprintf(BUF, PCAP_ERRBUF_SIZE + 128, "Filter not applied. Error while compiling: %s", pcap_geterr(handle));
					drawStatus(BUF);
				} else {
					if (pcap_setfilter(handle, &activeBPFFilter) == -1) {
						char BUF[PCAP_ERRBUF_SIZE + 128];
						snprintf(BUF, PCAP_ERRBUF_SIZE + 128, "Filter not applied. setfilter(): %s", pcap_geterr(handle));
						drawStatus(BUF);
					} else {
						activeBPFFilterName = newBPFFilterName;
					}
					isFilterUsed = TRUE;
				}
			}
			deviceDataLink = pcap_datalink(handle);
		}

#ifdef USE_SELECT
		{
			int pcap_fd = pcap_fileno(handle);
			int sel_ret;
			struct timeval timeout;
			fd_set set1;

			FD_ZERO(&set1);
			FD_SET(pcap_fd, &set1);
			timeout.tv_sec = 0;
			timeout.tv_usec = 500000;
			sel_ret = select(pcap_fd+1, &set1, NULL, NULL, &timeout);
			if (sel_ret > 0) {
				pcap_dispatch(handle, 10, (pcap_handler)dispatch_callback, NULL);
			}
		}
		
#else
		{
			packetReceived = FALSE;
			pcap_dispatch(handle, 10, (pcap_handler)dispatch_callback, NULL);
			if (!packetReceived)
				g_thread_yield();
		}
#endif
	}

	threadCount --;
	return NULL;
}

void    initDefaults() {
	ntop_resolv_entry *entry;
	entry = g_new0(ntop_resolv_entry, 1);
	entry->name = "UNKNOWNv4";
	entry->af = AF_INET;
	entry->addr.addr4.s_addr = 0x0;
	g_hash_table_insert(resolverCache, entry, entry);
	entry = g_new0(ntop_resolv_entry, 1);
	entry->name = "UNKNOWNv6";
	entry->af = AF_INET6;
	entry->addr.addr6.s6_addr32[0] = 0x0;
	entry->addr.addr6.s6_addr32[1] = 0x0;
	entry->addr.addr6.s6_addr32[2] = 0x0;
	entry->addr.addr6.s6_addr32[3] = 0x0;
	g_hash_table_insert(resolverCache, entry, entry);
	entry = g_new0(ntop_resolv_entry, 1);
	entry->name = "AGGREGATEDv4";
	entry->af = AF_INET;
	entry->addr.addr4.s_addr = htonl(0x01000000);
	g_hash_table_insert(resolverCache, entry, entry);
	entry = g_new0(ntop_resolv_entry, 1);
	entry->name = "AGGREGATEDv6";
	entry->af = AF_INET6;
	entry->addr.addr6.s6_addr32[0] = 0x0;
	entry->addr.addr6.s6_addr32[1] = 0x0;
	entry->addr.addr6.s6_addr32[2] = 0x0;
	entry->addr.addr6.s6_addr32[3] = htonl(0x01000000);
	g_hash_table_insert(resolverCache, entry, entry);
	configDeviceName = NULL;
}

int	config_parse_boolean(GScanner *s) {
	GTokenType tt;
	tt = g_scanner_get_next_token(s);
	if (tt != G_TOKEN_IDENTIFIER || (strcmp(s->value.v_identifier, "on") && strcmp(s->value.v_identifier,"off"))) {
		return -1;
	}
	return strcmp(s->value.v_identifier, "off")?TRUE:FALSE;
}

int     parse_aggregation(const char *agg) {
	if (strcmp(agg, "none") && strcmp(agg,"host") && strcmp(agg,"port")) {
		return AGG_UNKNOWN;
	}
	switch (*agg) {
		case 'n': return AGG_NONE;
		case 'h': return AGG_HOST;
		case 'p': return AGG_PORT;
	}
	return AGG_UNKNOWN;
}

int	config_parse_aggregation(GScanner *s) {
	GTokenType tt;
	tt = g_scanner_get_next_token(s);
	if (tt != G_TOKEN_IDENTIFIER) {
		return AGG_UNKNOWN;
	}
	return parse_aggregation(s->value.v_identifier);
}


void	readConfig() {
	FILE *f;
	GScanner *s;
	GHashTable *variables;
	char *homeDir;

	variables = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	if (!configFileName) {
		homeDir = getenv("HOME");
		if (!homeDir) {
			configFileName = ".jnettop";
		} else {
			configFileName = g_new0(char, strlen(homeDir) + 10);
			sprintf(configFileName, "%s/.jnettop", homeDir);
		}
	}

	f = fopen(configFileName, "r");
	if (!f) {
		fprintf(stderr, "Could not read/find config file %s: %s.\n", configFileName, strerror(errno));
		return;
	}

	s = g_scanner_new(NULL);
	g_scanner_input_file(s, fileno(f));
	while (!g_scanner_eof(s)) {
		GTokenType tt;
		int line;

		line = s->line;
		tt = g_scanner_get_next_token(s);
		if (tt == G_TOKEN_EOF) {
			break;
		}
		if (tt != G_TOKEN_IDENTIFIER) {
			fprintf(stderr, "Parse error on line %d: identifier expected.\n", line);
			exit(255);
		}
		if (!g_ascii_strcasecmp(s->value.v_identifier, "variable")) {
			char * variableName, * variableValue;
			tt = g_scanner_get_next_token(s);
			if (tt != G_TOKEN_STRING) {
				fprintf(stderr, "Parse error on line %d: variable name as string expected.\n", line);
				exit(255);
			}
			variableName = g_strdup(s->value.v_string);
			tt = g_scanner_get_next_token(s);
			if (tt != G_TOKEN_STRING) {
				fprintf(stderr, "Parse error on line %d: variable value as string expected.\n", line);
				exit(255);
			}
			variableValue = g_strdup(s->value.v_string);
			g_hash_table_insert(variables, variableName, variableValue);
			continue;
		}
		if (!g_ascii_strcasecmp(s->value.v_identifier, "rule")) {
			char * ruleName;
			char * c;
			GString *str;
			tt = g_scanner_get_next_token(s);
			if (tt != G_TOKEN_STRING) {
				fprintf(stderr, "Parse error on line %d: rule name as string expected.\n", line);
				exit(255);
			}
			ruleName = g_strdup(s->value.v_string);
			tt = g_scanner_get_next_token(s);
			if (tt != G_TOKEN_STRING) {
				fprintf(stderr, "Parse error on line %d: rule expression as string expected.\n", line);
				exit(255);
			}
			str = g_string_new("");
			for (c=s->value.v_string; *c; c++) {
				char * rightBracket;
				char * variableValue;
				if (*c == '$' && *(c+1) == '{') {
					rightBracket = strchr(c, '}');
					c += 2;
					if (!rightBracket) {
						fprintf(stderr, "Wrong variable substitution on line %d!\n", line);
						exit(255);
					}
					*rightBracket = '\0';
					variableValue = g_hash_table_lookup(variables, c);
					if (!variableValue) {
						fprintf(stderr, "Undefined variable %s on line %d!\n", c, line);
						exit(255);
					}
					g_string_append(str, variableValue);
					c = rightBracket;
				} else {
					g_string_append_c(str, *c);
				}
			}
			g_ptr_array_add(bpfFilters, ruleName);
			g_ptr_array_add(bpfFilters, str->str);
			g_string_free(str, FALSE);
			continue;
		}
		if (!g_ascii_strcasecmp(s->value.v_identifier, "interface")) {
			tt = g_scanner_get_next_token(s);
			if (tt != G_TOKEN_STRING) {
				fprintf(stderr, "Parse error on line %d: interface name as string expected.\n", line);
				exit(255);
			}
			configDeviceName = g_strdup(s->value.v_string);
			continue;
		}
		if (!g_ascii_strcasecmp(s->value.v_identifier, "promisc")) {
			int val = config_parse_boolean(s);
			if (val == -1) {
				fprintf(stderr, "Parse error on line %d: expecting on or off value.\n", line);
				exit(255);
			}
			if (onoffPromisc == -1)
				onoffPromisc = val;
			continue;
		}
		if (!g_ascii_strcasecmp(s->value.v_identifier, "local_aggregation")) {
			int val = config_parse_aggregation(s);
			if (val == AGG_UNKNOWN) {
				fprintf(stderr, "Parse error on line %d: expecting none or host or port.\n", line);
				exit(255);
			}
			if (localAggregation == AGG_UNKNOWN)
				localAggregation = val;
			continue;
		}
		if (!g_ascii_strcasecmp(s->value.v_identifier, "remote_aggregation")) {
			int val = config_parse_aggregation(s);
			if (val == AGG_UNKNOWN) {
				fprintf(stderr, "Parse error on line %d: expecting none or host or port.\n", line);
				exit(255);
			}
			if (remoteAggregation == AGG_UNKNOWN)
				remoteAggregation = val;
			continue;
		}
		if (!g_ascii_strcasecmp(s->value.v_identifier, "select_rule")) {
			char * ruleName;
			int i;
			tt = g_scanner_get_next_token(s);
			if (tt != G_TOKEN_STRING) {
				fprintf(stderr, "Parse error on line %d: rule name as string expected.\n", line);
				exit(255);
			}
			ruleName = g_strdup(s->value.v_string);
			for (i=0; i<bpfFilters->len/2; i++) {
				char * iName;
				iName = (char *)g_ptr_array_index(bpfFilters, i*2);
				if (!strcmp(iName, ruleName)) {
					newBPFFilter = g_ptr_array_index(bpfFilters, i*2 + 1);
					newBPFFilterName = (char *) g_ptr_array_index(bpfFilters, i*2 );
					break;
				}
			}
			if (i<bpfFilters->len/2)
				continue;
			fprintf(stderr, "Parse error on line %d: rule %s not defined so far.\n", line, ruleName);
			exit(255);
		}
	}

	g_hash_table_destroy(variables);

}

int main(int argc, char ** argv) {
	int a;
	char * deviceName = NULL;
	char * selectRuleName = NULL;

	onoffContentFiltering = TRUE;
	onoffBitValues = FALSE;
	onoffPromisc = -1;
	localAggregation = AGG_UNKNOWN;
	remoteAggregation = AGG_UNKNOWN;
	
	for (a=1; a<argc; a++) {
		if (!strcmp(argv[a], "-v") || !strcmp(argv[a], "--version")) {
			printf(PACKAGE_STRING "\nWritten by Jakub Skopal <j@kubs.cz>\n\nSee copyright in the COPYING file.\n");
			exit(0);
		}
		if (!strcmp(argv[a], "-h") || !strcmp(argv[a], "--help")) {
			printf(	"Usage: jnettop [-hv] [-i interface] [-d filename]\n"
				"\n"
				"    -h, --help             display this help message\n"
				"    -v, --version          display version information\n\n"
				"    -b, --bit-units        show BPS in bits per second, not bytes per second\n"
				"    -c, --content-filter   disable content filtering\n"
				"    -d, --debug filename   write debug information into file\n"
				"    -f, --config-file name reads configuration from file. defaults to ~/.jnettop\n"
				"    -i, --interface name   capture packets on specified interface\n"
				"    --local-aggr arg       set local aggregation to none/host/port\n"
				"    -p, --promiscuous      enable promisc mode on the devices\n"
				"    --remote-aggr arg      set remote aggregation to none/host/port\n"
				"    -s, --select-rule rule selects one of the rules defined in config file\n"
				"                           by it's name\n"
				"    -x, --filter rule      allows for specification of custom filtering rule\n"
				"                           this follows tcpdump(1) syntax. don't forget to\n"
				"                           enclose the filter into quotes when running from shell\n"
				"\n"
				"Report bugs to <j@kubs.cz>\n");
			exit(0);
		}
		if (!strcmp(argv[a], "-b") || !strcmp(argv[a], "--bit-units")) {
			onoffBitValues = TRUE;
			continue;
		}
		if (!strcmp(argv[a], "-c") || !strcmp(argv[a], "--content-filter")) {
			onoffContentFiltering = FALSE;
			continue;
		}
		if (!strcmp(argv[a], "-i") || !strcmp(argv[a], "--interface")) {
			if (a+1>=argc) {
				fprintf(stderr, "%s switch requires argument\n", argv[a]);
				exit(255);
			}
			deviceName = argv[++a];
			continue;
		}
		if (!strcmp(argv[a], "-s") || !strcmp(argv[a], "--select-rule")) {
			if (a+1>=argc) {
				fprintf(stderr, "%s switch requires argument\n", argv[a]);
				exit(255);
			}
			selectRuleName = argv[++a];
			continue;
		}
		if (!strcmp(argv[a], "-d") || !strcmp(argv[a], "--debug")) {
			if (a+1>=argc) {
				fprintf(stderr, "%s switch requires filename to debug to as an argument\n", argv[a]);
				exit(255);
			}
			debugFile = fopen(argv[++a], "w");
			if (!debugFile) {
				perror("Could not open debug file");
				exit(255);
			}
			continue;
		}
		if (!strcmp(argv[a], "-f") || !strcmp(argv[a], "--config-file")) {
			if (a+1>=argc) {
				fprintf(stderr, "%s switch required argument\n", argv[a]);
				exit(255);
			}
			configFileName = argv[++a];
			continue;
		}
		if (!strcmp(argv[a], "-x") || !strcmp(argv[a], "--filter")) {
			const char *ret;
			if (a+1>=argc) {
				fprintf(stderr, "%s switch requires argument\n", argv[a]);
				exit(255);
			}
			commandLineRule = argv[++a];
			ret = validateBPFFilter(commandLineRule);
			if (ret) {
				fprintf(stderr, "Error compiling rule: %s\n", ret);
				exit(255);
			}
			newBPFFilterName = commandLineRule;
			newBPFFilter = commandLineRule;
			continue;
		}
		if (!strcmp(argv[a], "-p") || !strcmp(argv[a], "--promiscuous")) {
			onoffPromisc = TRUE;
			continue;
		}
		if (!strcmp(argv[a], "--local-aggr")) {
			if (a+1>=argc || (localAggregation = parse_aggregation(argv[++a]))==-1) {
				fprintf(stderr, "%s switch requires none, host or port as an argument\n", argv[a]);
				exit(255);
			}
			continue;
		}
		if (!strcmp(argv[a], "--remote-aggr")) {
			if (a+1>=argc || (remoteAggregation = parse_aggregation(argv[++a]))==-1) {
				fprintf(stderr, "%s switch requires none, host or port as an argument\n", argv[a]);
				exit(255);
			}
			continue;
		}
		fprintf(stderr, "Unknown argument: %s\n", argv[a]);
		exit(255);
	}

	g_thread_init(NULL);

	packetQueue = g_queue_new();
	packetQueueCond = g_cond_new();
	packetQueueMutex = g_mutex_new();

	resolverCache = g_hash_table_new((GHashFunc)hashResolvEntry, (GEqualFunc)compareResolvEntry);
	resolverCacheMutex = g_mutex_new();

	streamTable = g_hash_table_new((GHashFunc)hashStream, (GEqualFunc)compareStream);
	streamTableMutex = g_mutex_new();

	streamArray = g_ptr_array_new();
	streamArrayMutex = g_mutex_new();

	displayStreamsMutex = g_mutex_new();
	freePacketStackMutex = g_mutex_new();

	bpfFilters = g_ptr_array_new();

	statusMutex = g_mutex_new();

	initDefaults();
	readConfig();

	if (onoffPromisc == -1)
		onoffPromisc = FALSE;
	if (localAggregation == AGG_UNKNOWN)
		localAggregation = AGG_NONE;
	if (remoteAggregation == AGG_UNKNOWN)
		remoteAggregation = AGG_NONE;
	if (selectRuleName) {
		int i;
		for (i=0; i<bpfFilters->len/2; i++) {
			char * iName;
			iName = (char *)g_ptr_array_index(bpfFilters, i*2);
			if (!strcmp(iName, selectRuleName)) {
				newBPFFilter = g_ptr_array_index(bpfFilters, i*2 + 1);
				newBPFFilterName = (char *) g_ptr_array_index(bpfFilters, i*2 );
				break;
			}
		}
		if (i>=bpfFilters->len/2) {
			fprintf(stderr, "Rule '%s' specified on the command line is not defined.\n", selectRuleName);
			exit(255);
		}
	}

	lookupDevices();
	
	newDevice = NULL;

	if (!devices_count) {
			if (!deviceName && !configDeviceName) {
				fprintf(stderr, "Autodiscovery found no devices. Specify device you want to watch with -i parameter\n");
				exit(255);
			} else if (deviceName) {
				createDevice(deviceName);
			} else {
				createDevice(configDeviceName);
			}
	} else if (deviceName || configDeviceName) {
		int i;
		if (!deviceName && configDeviceName)
			deviceName = configDeviceName;
		for (i=0; i<devices_count; i++) {
			if (!strcmp(devices[i].name, deviceName)) {
				newDevice = devices + i;
				break;
			}
		}
		if (!newDevice)
			createDevice(deviceName);
	}

	if (!newDevice)
		newDevice = devices;

	checkDevices();

	initscr();
	cbreak();
	noecho();
	nonl();
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);
	nodelay(stdscr, TRUE);

	resolverThreadPool = g_thread_pool_new((GFunc)resolverThreadFunc, NULL, 5, FALSE, NULL);

	while (newDevice) {

		clearStatistics();
		activeDevice = newDevice;
		newDevice = NULL;
		displayStreams = NULL;
		displayStreamsCount = 0;

		g_get_current_time(&startTime);
		totalBytes = 0;
		totalSrcBytes = 0;
		totalDstBytes = 0;
		totalPackets = 0;
		totalSrcBPS = 0;
		totalDstBPS = 0;
		totalBPS = 0;

		activeLines = 0;
		activeColumns = 0;

		if (statusMessage) {
			g_free(statusMessage);
			statusMessage = NULL;
		}

		clear();
		drawScreen();
		
		snifferThread = g_thread_create((GThreadFunc)snifferThreadFunc, NULL, TRUE, NULL);
		sorterThread = g_thread_create((GThreadFunc)sorterThreadFunc, NULL, FALSE, NULL);
		processorThread = g_thread_create((GThreadFunc)processorThreadFunc, NULL, FALSE, NULL);
		displayThread = g_thread_create((GThreadFunc)displayThreadFunc, NULL, TRUE, NULL);
		g_thread_join(displayThread);

		if (!newDevice && !newBPFFilter) {
			// In case we're not switching to another device, we can happily finish
			// after our display thread dies. (mind the endwin())
			break;
		}

		g_thread_join(snifferThread);
		
		while (threadCount) {
			g_thread_yield();
		}
	}

	if (debugFile) {
		fclose(debugFile);
	}

	endwin();
	return 0;
}
