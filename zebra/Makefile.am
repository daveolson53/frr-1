include ../common.am

## Process this file with automake to produce Makefile.in.

AM_CPPFLAGS = -I.. -I$(top_srcdir) -I$(top_srcdir)/lib -I$(top_builddir)/lib
DEFS = @DEFS@ -DSYSCONFDIR=\"$(sysconfdir)/\"
INSTALL_SDATA=@INSTALL@ -m 600

LIBCAP = @LIBCAP@

ipforward = @IPFORWARD@
if_method = @IF_METHOD@
rt_method = @RT_METHOD@
rtread_method = @RTREAD_METHOD@
kernel_method = @KERNEL_METHOD@
ioctl_method = @IOCTL_METHOD@
mpls_method = @MPLS_METHOD@

otherobj = $(ioctl_method) $(ipforward) $(if_method) \
	$(rt_method) $(rtread_method) $(kernel_method) $(mpls_method)

AM_CFLAGS = $(WERROR)

sbin_PROGRAMS = zebra
module_LTLIBRARIES =

zebra_SOURCES = \
	zebra_memory.c \
	zserv.c main.c interface.c connected.c zebra_rib.c zebra_routemap.c \
	redistribute.c debug.c rtadv.c zebra_vty.c \
	irdp_main.c irdp_interface.c irdp_packet.c router-id.c \
	zebra_ptm.c zebra_rnh.c zebra_ptm_redistribute.c \
	zebra_ns.c zebra_vrf.c zebra_static.c zebra_mpls.c zebra_mpls_vty.c \
	zebra_mroute.c \
	label_manager.c \
	zebra_l2.c \
	zebra_vxlan.c \
	# end

zebra_vty.o: zebra_vty_clippy.c

noinst_HEADERS = \
	zebra_memory.h \
	connected.h ioctl.h rib.h rt.h zserv.h redistribute.h debug.h rtadv.h \
	interface.h ipforward.h irdp.h router-id.h kernel_socket.h \
	rt_netlink.h zebra_fpm_private.h zebra_rnh.h \
	zebra_ptm_redistribute.h zebra_ptm.h zebra_routemap.h \
	zebra_ns.h zebra_vrf.h ioctl_solaris.h zebra_static.h zebra_mpls.h \
	kernel_netlink.h if_netlink.h zebra_mroute.h label_manager.h \
	zebra_l2.h zebra_vxlan_private.h zebra_vxlan.h

zebra_LDADD = $(otherobj) ../lib/libfrr.la $(LIBCAP)

zebra_DEPENDENCIES = $(otherobj)

if SNMP
module_LTLIBRARIES += zebra_snmp.la
endif
zebra_snmp_la_SOURCES = zebra_snmp.c
zebra_snmp_la_CFLAGS = $(WERROR) $(SNMP_CFLAGS)
zebra_snmp_la_LDFLAGS = -avoid-version -module -shared -export-dynamic
zebra_snmp_la_LIBADD = ../lib/libfrrsnmp.la

if FPM
module_LTLIBRARIES += zebra_fpm.la
endif
zebra_fpm_la_LDFLAGS = -avoid-version -module -shared -export-dynamic
zebra_fpm_la_LIBADD = $(Q_FPM_PB_CLIENT_LDOPTS)
zebra_fpm_la_SOURCES = zebra_fpm.c
if HAVE_NETLINK
zebra_fpm_la_SOURCES += zebra_fpm_netlink.c
endif
if HAVE_PROTOBUF
zebra_fpm_la_SOURCES += zebra_fpm_protobuf.c
if DEV_BUILD
zebra_fpm_la_SOURCES += zebra_fpm_dt.c
endif
endif


EXTRA_DIST = if_ioctl.c if_ioctl_solaris.c if_netlink.c \
        if_sysctl.c ipforward_proc.c \
	ipforward_solaris.c ipforward_sysctl.c rt_netlink.c \
	rt_socket.c rtread_netlink.c rtread_sysctl.c \
	rtread_getmsg.c kernel_socket.c kernel_netlink.c \
	ioctl.c ioctl_solaris.c \
	zebra_mpls_netlink.c zebra_mpls_openbsd.c zebra_mpls_null.c \
	GNOME-SMI GNOME-PRODUCT-ZEBRA-MIB

client : client_main.o ../lib/libfrr.la
	$(CC) -g -o client client_main.o ../liblzebra.la $(LIBS) $(LIB_IPV6)

frrconfdir = $(sysconfdir)

examplesdir = $(exampledir)
dist_examples_DATA = zebra.conf.sample
