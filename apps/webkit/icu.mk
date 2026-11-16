# ICU's libraries for ThreadOS, compiled straight from its sources
#
# Called from the Makefile once the release is unpacked (the list of
# files is known only then): every source of 'common' and 'i18n' with
# the flags of the C++ programs on musl, into two static libraries, and
# the data - trimmed by apps/webkit/icu-data.py, turned into an object
# by ICU's own genccode - into a third. ICU's configure is left out:
# it would want to link test programs the way a hosted system does.
#
#   make -f apps/webkit/icu.mk SRC=<icu/source> OUT=<dir> DAT=<.dat>
#        GENCCODE=<host genccode> CXX=... CXXFLAGS=... CC=... AR=...

COMMON := $(wildcard $(SRC)/common/*.cpp)
I18N := $(wildcard $(SRC)/i18n/*.cpp)
COMMON_O := $(patsubst $(SRC)/common/%.cpp,$(OUT)/common/%.o,$(COMMON))
I18N_O := $(patsubst $(SRC)/i18n/%.cpp,$(OUT)/i18n/%.o,$(I18N))

# Static, without loadable plugins or data files on a disk: everything
# ICU needs is linked in
ICU_DEFS := -DU_STATIC_IMPLEMENTATION -DU_ENABLE_DYLOAD=0 \
            -DU_HAVE_STRTOD_L=0 -DU_HAVE_XLOCALE_H=0 \
            -DUCONFIG_NO_FILE_IO=0 -DU_CHARSET_IS_UTF8=1

all: $(OUT)/libicuuc.a $(OUT)/libicui18n.a $(OUT)/libicudata.a

$(OUT)/common/%.o: $(SRC)/common/%.cpp | $(OUT)/common
	$(CXX) $(CXXFLAGS) $(ICU_DEFS) -DU_COMMON_IMPLEMENTATION \
	    -I$(SRC)/common -c $< -o $@

$(OUT)/i18n/%.o: $(SRC)/i18n/%.cpp | $(OUT)/i18n
	$(CXX) $(CXXFLAGS) $(ICU_DEFS) -DU_I18N_IMPLEMENTATION \
	    -I$(SRC)/common -I$(SRC)/i18n -c $< -o $@

$(OUT)/libicuuc.a: $(COMMON_O)
	rm -f $@ && $(AR) rcs $@ $^

$(OUT)/libicui18n.a: $(I18N_O)
	rm -f $@ && $(AR) rcs $@ $^

# The data as an object: genccode writes it out as assembly, a symbol
# named for the version (icudt78_dat) that ICU looks for
$(OUT)/libicudata.a: $(DAT)
	rm -rf $(OUT)/data && mkdir -p $(OUT)/data
	$(GENCCODE) -a gcc -e $(basename $(notdir $(DAT:l.dat=))) \
	    -d $(OUT)/data $(DAT)
	$(CC) -c $(OUT)/data/*.S -o $(OUT)/data/icudata.o
	rm -f $@ && $(AR) rcs $@ $(OUT)/data/icudata.o

$(OUT)/common $(OUT)/i18n:
	mkdir -p $@
