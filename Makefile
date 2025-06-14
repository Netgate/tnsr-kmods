#
# Copyright 2021-2022 Rubicon Communications, LLC.
#

OS_ID = $(shell grep '^ID=' /etc/os-release | cut -f2- -d= | sed -e 's/\"//g')
ifeq ($(filter ubuntu debian,$(OS_ID)),$(OS_ID))
    PKG_TYPE := deb
    OS_LIB_DIR := lib
else
$(error Can not determine OS.)
endif

# Choices are: 'x86', 'aarch64'
TRGT_ARCH     ?= x86

export BR=$(CURDIR)/build-root

DIST_FILE = $(BR)/tnsr-kmods-$(shell extras/scripts/version).tar
DIST_SUBDIR = tnsr-kmods-$(shell extras/scripts/version | cut -f1 -d-)

dist:
	@mkdir -p $(BR)
	@if git rev-parse 2> /dev/null ; then \
	    git archive \
	      --prefix=$(DIST_SUBDIR)/ \
	      --format=tar \
	      -o $(DIST_FILE) \
	    HEAD ; \
	    git describe > $(BR)/.version ; \
	else \
	    (cd .. ; tar -cf $(DIST_FILE) $(DIST_SUBDIR) --exclude=*.tar) ; \
	    extra/scripts/version > $(BR)/.version ; \
	fi
	@tar --append \
	  --file $(DIST_FILE) \
	  --transform='s,.*/.version,$(DIST_SUBDIR)/build-root/scripts/.version,' \
	  $(BR)/.version
	@for f in $$(git status --porcelain | grep '^[DR]' | awk '{print $$2}'); do \
	  echo "Removing $${f}"; \
	  tar --delete --file $(DIST_FILE) $(DIST_SUBDIR)/$${f}; \
	done
	@for f in $$(git status --porcelain -u | grep -v '^[DR]' | awk '{print $$2}'); do \
	  echo "Adding $${f}"; \
	  tar --append --file $(DIST_FILE) \
	  --transform="s,$${f},$(DIST_SUBDIR)/$${f}," \
	  $${f}; \
	done
	@for f in $$(git status --porcelain -u | grep '^R' | awk '{print $$4}'); do \
	  echo "Adding $${f}"; \
	  tar --append --file $(DIST_FILE) \
	  --transform="s,$${f},$(DIST_SUBDIR)/$${f}," \
	  $${f}; \
	done
	@$(RM) $(BR)/.version $(DIST_FILE).xz
	@xz -v --threads=0 $(DIST_FILE)
	@$(RM) $(BR)/tnsr-kmods-latest.tar.xz
	@ln -rs $(DIST_FILE).xz $(BR)/tnsr-kmods-latest.tar.xz
