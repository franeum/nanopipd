
# library name
lib.name = bangmessage

# input source file (class name == source file basename)
class.sources = bangmessage.c

# all extra files to be included in binary distribution of the library
datafiles =


ldlibs = -lpthread -lwiringPi

# include Makefile.pdlibbuilder from submodule directory 'pd-lib-builder'
# PDLIBBUILDER_DIR=../pd-lib-builder/
# include $(PDLIBBUILDER_DIR)/Makefile.pdlibbuilder
include Makefile.pdlibbuilder

shared.sources = wiringPi
