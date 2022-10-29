#
#     __________ ________  _____
#    |__    ___/|   ____/ /  _  \ the fastest packet injector.
#      |    |   |____  \ /  /_\  \
#      |    |   /       \\  \_/   \
#      |____|  /________/\\_______/
#
# NOTE: I've got rid of autoconf 'cause there is a dependency there.
#       This way you don't need anything other than this makefile to
#       compile the project.

VERSION=5.8.7a

#CC=gcc
#
# OBS: T50 will compile only on GCC 5 or above. If you are feeling lucky,
#      try GCC 4.9 or CLANG... But I don't recomended.
#

LD=$(CC)

INCLUDEDIR=src/include
CFLAGS=-std=gnu11 -I $(INCLUDEDIR)
LDFLAGS=
LDLIBS=

# Just define DEBUG environment var to compile for debugging:
#
# $ DEBUG=1 make
#

ifdef DEBUG
	CFLAGS += -O0 -g
else
  # Optimization level 2 (better results) and no canaries.
  CFLAGS += -O2 -DNDEBUG -ffast-math

  # strip symbols and turn more linker optimizations on (if available).
  LDFLAGS += -s -O2

  ARCHITECTURE = $(shell arch)

  IS_GCC = $(shell $(CC) -v 2>&1 | sed -nE 's/^(.+) version.+$$/\1/p')
  CC_VERSION = $(shell $(CC) -v 2>&1 | sed -nE 's/^.+version (.).+$$/\1/p')

  # Options for x86-64
  ifeq ($(ARCHITECTURE),x86_64)
    CFLAGS += -march=native -mtune=native -ftree-vectorize -flto -fno-stack-protector

		# Avoid CTE on Intel Platforms.
		# Currently works on GCC 9+ (don't know if works on clang).
    ifeq ($(IS_GCC),gcc)
      ifeq ($(shell expr $(CC_VERSION) \>= 9),1)
        CFLAGS += -fcf-protection=none
      endif
    endif

    LDFLAGS += -flto
  endif
  # Options for i386
  ifeq ($(ARCHITECTURE),i686)
    CFLAGS += -march=native -mtune=native -flto -fno-stack-protector

		# Avoid CTE on Intel Platforms.
		# Currently works on GCC 9+ (don't know if works on clang).
    ifeq ($(IS_GCC),gcc)
      ifeq ($(shell expr $(CC_VERSION) \>= 9),1)
        CFLAGS += -fcf-protection=none
      endif
    endif

    LDFLAGS += -flto
  endif

  # TODO: tunning for arm-cortex-a7? (Raspberry PI?)
  # TODO: aarch64?!
  # Options for ARMv7-a
  ifneq ($(findstring armv7,$(ARCHITECTURE)),)
    CFLAGS += -march=armv7-a -fno-stack-protector -flto
    LDFLAGS += -flto
  endif

  # Options for ARMv8-a
  ifneq ($(findstring armv8,$(ARCHITECTURE)),)
    CFLAGS += -march=armv8-a -fno-stack-protector -flto
    LDFLAGS += -flto
  endif
endif

# Added to use ANSI CSI codes (beautifier).
ifdef USE_ANSI
  CFLAGS += -DUSE_ANSI
endif

EXECUTABLE=bin/t50

OBJECTS=\
src/cidr.o \
src/cksum.o \
src/config.o \
src/errors.o \
src/main.o \
src/memalloc.o \
src/modules.o \
src/netio.o \
src/randomizer.o \
src/shuffle.o \
src/usage.o \
src/help/egp_help.o \
src/help/eigrp_help.o \
src/help/general_help.o \
src/help/gre_help.o \
src/help/icmp_help.o \
src/help/igmp_help.o \
src/help/ip_help.o \
src/help/ipsec_help.o \
src/help/ospf_help.o \
src/help/rip_help.o \
src/help/rsvp_help.o \
src/help/tcp_udp_dccp_help.o \
src/modules/dccp.o \
src/modules/egp.o \
src/modules/eigrp.o \
src/modules/gre.o \
src/modules/icmp.o \
src/modules/igmpv1.o \
src/modules/igmpv3.o \
src/modules/ip.o \
src/modules/ipsec.o \
src/modules/ospf.o \
src/modules/ripv1.o \
src/modules/ripv2.o \
src/modules/rsvp.o \
src/modules/tcp.o \
src/modules/udp.o

.PHONY: all clean distclean dist install uninstall

all: $(EXECUTABLE)

# Now we'll compile to ./bin/ directory!
# Explicit rule needed 'cause we have multiple objects.
$(EXECUTABLE): $(OBJECTS)
	$(LD) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Implicit rules (all .c files will be compiled!)
src/%.o: src/%.c
src/help/%.o: src/help/%.c
src/modules/%.o: src/modules/%.c

# 'clean' only deletes the object files.
clean:
	@echo 'Deleting .o files...'
	-find src/ -type f -name '*.o' -delete

# distclean delete the object files AND the executable.
distclean: clean
	-rm $(EXECUTABLE) dist/*.gz dist/*.asc

# Shortcut to check if user has root privileges.
define checkifroot
	if [ `id -u` -ne 0 ]; then \
		echo 'Need root privileges!'; \
		exit 1; \
	fi
endef

# install and uninstall rules are very simple!
install:
	@$(call checkifroot)
	@if [ ! -e "$(EXECUTABLE)" ]; then \
		echo "Try 'make' first."; \
		exit 1; \
	fi;
	cp bin/t50 /sbin/; cp doc/t50.8 /usr/share/man/man8/; \
	chown root: /sbin/t50 /usr/share/man/man8; \
	chmod 4750 /sbin/t50; \
	gzip -9 /usr/share/man/man8/t50.8; \
	chmod 0644 /usr/share/man/man8/t50.8.gz

uninstall:
	@$(call checkifroot)
	rm /sbin/t50 /usr/share/man/man8/t50.8.gz

# Needed to build the project source tarball (no signature generation here).
dist: distclean
	tar -czvf dist/t50-$(VERSION).tar.gz --exclude=*.tar.gz --exclude=*.asc *
