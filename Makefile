# SA-Exim can be built standalone as a loadable module with this Makefile
# or you can copy sa-exim.c over exim's local_scan.c file if you want to
# statically build it into exim
#

VERSION=$(shell cat version)

# The idea is that you don't have to edit these values, you can override
# them on the command line:
# make SACONF=/etc/exim/sa-exim.conf LDFLAGS="-shared -fPIC" CC=cc
CC=gcc
CFLAGS=-O2 -Wall
LDFLAGS=-shared
SACONF=/etc/exim4/sa-exim.conf
SPAMC=/usr/bin/spamc


# I place the directory in exim/debian/local_scan. Adjust the path as needed
# Actually, we will also look for the versions supplied with this source
# if we can't find the exim source
EXIM_SRC= ../../src
EXIM_SRC_LOCAL = ./eximinc
SUFF=-$(VERSION)

SAFLAGS=-DSPAMASSASSIN_CONF=\"$(SACONF)\" -DSPAMC_LOCATION=\"$(SPAMC)\"
BUILDCFLAGS=-I$(EXIM_SRC) -I$(EXIM_SRC_LOCAL) -DDLOPEN_LOCAL_SCAN $(SAFLAGS) $(CFLAGS)

SONAME=$(subst .so,$(SUFF).so,sa-exim.so)

DOCS=sa.html CHANGELOG ACKNOWLEDGEMENTS
OBJECTS=$(SONAME) accept.so sa-exim_short.conf $(DOCS)
OTHERTARGETS=sa-exim.h

all: $(OBJECTS)

docs: $(DOCS)
	

$(SONAME) : sa-exim.c sa-exim.h
	@echo "Building $@"
	$(CC) $(BUILDCFLAGS) $(LDFLAGS) -o $@ $<
	chmod a+rx $(SONAME)

accept.so: accept.c
	@echo "Building $@"
	$(CC) $(BUILDCFLAGS) $(LDFLAGS) -o $@ $<
	chmod a+rx $@

ACKNOWLEDGEMENTS: Acknowledgements.html
	@echo "Generating $@"
	@links -dump -codepage UTF-8 $< > $@

CHANGELOG: Changelog.html
	@echo "Generating $@"
	@links -dump $< > $@

sa.html: Changelog.html Acknowledgements.html sa.html.template
	@echo "Generating $@"
	@bash -c 'sed "/<Changelog>/,$$ d" < sa.html.template; cat Changelog.html; sed "1,/<\/Changelog>/ d; /<Acknowledgements>/,$$ d" < sa.html.template; cat Acknowledgements.html; sed "1,/<\/Acknowledgements>/ d" < sa.html.template' > sa.html

sa-exim_short.conf: sa-exim.conf
	@cat sa-exim.conf | sed "/# --- snip ---/,$$ d" > sa-exim_short.conf
	@cat sa-exim.conf | grep -v "^#" | tr '\012' 'ÿ' | sed "s/ÿÿÿ*/ÿÿ/g" | tr 'ÿ' '\012' >> sa-exim_short.conf

sa-exim.h: sa-exim.c version
ifdef SOURCE_DATE_EPOCH
	echo "char *version=\"`cat version` (built `LC_ALL=C date --utc -R --date=@$${SOURCE_DATE_EPOCH}`)\";" > sa-exim.h
else
	echo "char *version=\"`cat version` (built `date -R 2>/dev/null || date`)\";" > sa-exim.h
endif

clean:	
	@-rm -rf $(OBJECTS) $(DOCS) $(OTHERTARGETS) build-stamp configure-stamp debian/sa-exim debian/sa-exim.postrm.debhelper debian/sa-exim.substvars debian/files 2>/dev/null

deb:	../sa-exim_$(VERSION).orig.tar.gz debian/*
	@make clean
	@dpkg-buildpackage -uc -us -sd -rfakeroot
	@make clean

../sa-exim_$(VERSION).orig.tar.gz: * */*
	@make clean
	@( cd ..; tar chvzf sa-exim_$(VERSION).orig.tar.gz sa-exim-$(VERSION) )

# This didn't work too well, I'll just ship the source with the debian tree
#deb:	../sa-exim_$(VERSION).orig.tar.gz debian/rules
#	@make clean
#	@dpkg-buildpackage -uc -us -sd -rfakeroot
#
#
#../sa-exim_$(VERSION).tar.gz: * */*
#	@make clean
#	@if [ -d debian ]; then echo "Can't rebuild $@ with debian tree unpacked, please remove it"; exit 1; fi
#	@( cd ..; tar chvzf sa-exim_$(VERSION).tar.gz sa-exim-$(VERSION) )
#
#
#../sa-exim_$(VERSION).orig.tar.gz: ../sa-exim_$(VERSION).tar.gz
#	if [ -e ../sa-exim-$(VERSION).tar.gz ] ; then \
#	    cp -a ../sa-exim-$(VERSION).tar.gz ../sa-exim_$(VERSION).orig.tar.gz ; \
#        else \
#	    wget http://marc.merlins.org/linux/sa-exim-$(VERSION).tar.gz; \
#	    mv sa-exim-$(VERSION).tar.gz ../sa-exim_$(VERSION).orig.tar.gz; \
#        fi
#
#
#debian/rules:
#	@wget http://marc.merlins.org/linux/exim/files/debian/sa-exim_diff.gz
#	@zcat sa-exim_diff.gz | patch -s -p1
#	@/bin/rm sa-exim_diff.gz
#	@chmod 755 debian/rules
#

