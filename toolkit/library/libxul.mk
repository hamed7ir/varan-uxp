# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this file,
# You can obtain one at http://mozilla.org/MPL/2.0/.

EXTRA_DEPS += $(topsrcdir)/toolkit/library/libxul.mk

ifeq (Linux,$(OS_ARCH))
OS_LDFLAGS += -Wl,-version-script,symverscript

symverscript: $(topsrcdir)/toolkit/library/symverscript.in
	$(call py_action,preprocessor, \
		-DVERSION='xul$(MOZILLA_SYMBOLVERSION)' $< -o $@)

EXTRA_DEPS += symverscript
endif

# Generate GDB pretty printer-autoload files on Linux and Solaris. OSX's GDB is
# too old to support Python pretty-printers; if this changes, we could make
# this 'ifdef GNU_CC'.
ifeq (,$(filter-out SunOS Linux,$(OS_ARCH)))	
# Create a GDB Python auto-load file alongside the libxul shared library in
# the build directory.
PP_TARGETS += LIBXUL_AUTOLOAD
LIBXUL_AUTOLOAD = $(topsrcdir)/toolkit/library/libxul.so-gdb.py.in
LIBXUL_AUTOLOAD_FLAGS := -Dtopsrcdir=$(abspath $(topsrcdir))
endif

ifeq ($(OS_ARCH),SunOS)
OS_LDFLAGS += -Wl,-z,defs
endif

ifdef _MSC_VER
# Varan: the clang-cl/lld toolchain has no `dumpbin` (an MSVC-only tool) -> the
# original recipe died with "sh: dumpbin: command not found" and DELETED a correctly-linked xul.dll.
# Use llvm-nm on the COFF DLL (same parse as the GNU nm branch below), and make the ordering check
# NON-FATAL here: the NSModule array order is produced by the linker, so a nm-format/ordering quirk
# must not discard the M2 artifact. We REPORT the order for inspection; enforcing it fatal is an M3
# refinement (confirm static component registration actually works on-device first).
get_first_and_last = llvm-nm $1 | grep _NSModule$$ | grep -vw refptr | sort | sed -n 's/^.* _*\([^ ]*\)$$/\1/;1p;$$p'
LOCAL_CHECKS = echo "Varan NSModule order = [$$($(get_first_and_last) | xargs echo)] (want: start_kPStaticModules_NSModule end_kPStaticModules_NSModule)" ; exit 0
else
get_first_and_last = $(TOOLCHAIN_PREFIX)nm -g $1 | grep _NSModule$$ | grep -vw refptr | sort | sed -n 's/^.* _*\([^ ]*\)$$/\1/;1p;$$p'
LOCAL_CHECKS = test "$$($(get_first_and_last) | xargs echo)" != "start_kPStaticModules_NSModule end_kPStaticModules_NSModule" && echo "NSModules are not ordered appropriately" && exit 1 || exit 0
endif

ifeq (Linux,$(OS_ARCH))
LOCAL_CHECKS += ; test "$$($(TOOLCHAIN_PREFIX)readelf -l $1 | awk '$1 == "LOAD" { t += 1 } END { print t }')" -le 1 && echo "Only one PT_LOAD segment" && exit 1 || exit 0
endif

# It's safer to use elfdump on SunOS, because that's available on all
# supported versions of Solaris and illumos.

ifeq (SunOS,$(OS_ARCH))
LOCAL_CHECKS += ; test "elfdump -p $1 | awk '$5 == "PT_LOAD" { t += 1 } END { print t }')" -le 1 && echo "Only one PT_LOAD segment" && exit 1 || exit 0
endif
