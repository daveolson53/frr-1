## Process this file with automake to produce Makefile.in.

SUBDIRS = lib qpb fpm @ZEBRA@ @LIBRFP@ @RFPTEST@ \
	 @BGPD@ @RIPD@ @RIPNGD@ @OSPFD@ @OSPF6D@ @LDPD@ \
         @ISISD@ @PIMD@ @NHRPD@ @EIGRPD@ @BABELD@ \
	 @WATCHFRR@ @VTYSH@ @OSPFCLIENT@ @DOC@ m4 @pkgsrcdir@ \
         redhat @SOLARIS@ tests tools snapcraft

DIST_SUBDIRS = lib qpb fpm zebra bgpd ripd ripngd ospfd ospf6d ldpd \
	  isisd watchfrr vtysh ospfclient doc m4 pkgsrc redhat tests \
	  solaris pimd nhrpd eigrpd bgpd/rfp-example/librfp \
	  bgpd/rfp-example/rfptest tools snapcraft babeld python \
	  # end

EXTRA_DIST = aclocal.m4 SERVICES REPORTING-BUGS \
	update-autotools \
	vtysh/Makefile.in vtysh/Makefile.am \
	tools/rrcheck.pl tools/rrlookup.pl tools/zc.pl \
	tools/zebra.el tools/multiple-bgpd.sh

ACLOCAL_AMFLAGS = -I m4

noinst_HEADERS = defaults.h
