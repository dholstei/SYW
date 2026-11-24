# 
#	SYW - Show Your Work
#	Programmer: Danny Holstein
#
LOG?=Install.log	#	Use Linux "logger" command to send logging info to this file
ifeq (null,$(shell if ls -l /dev/log  2>/dev/null > /dev/null ; then echo system; else echo null; fi))
	LOGGER?={ read LOGMSG && echo "[$@: `date`]" $$LOGMSG | tee -a $(LOG); }
else
	LOGGER?=logger --tag "[$@: `date`]" -s 2>&1 | tee -a $(LOG)
endif
CXX=g++
CXXFLAGS=$(DEBUG) -std=c++17 -fPIC
QT_MODULES := Qt6Core Qt6Gui Qt6Widgets


INCLUDES:=-I/usr/include/libxml2
INCLUDES:=$(INCLUDES) -I/usr/include
INCLUDES:=$(INCLUDES) -I./ -I../XmlCls -I../cpp-base64
INCLUDES:=$(INCLUDES) $(shell pkg-config --cflags $(QT_MODULES))
LDFLAGS=$(DEBUG)
LDLIBS:=-lpthread -lxml2  $(shell pkg-config --libs $(QT_MODULES))
LIBXML:=../XmlCls/XmlCls.a ../XmlCls/XmlClsLib.so ../cpp-base64/libBase64.a
# ifeq ($(STATIC),)
# else
# endif

ifeq ($(DEBUG),)
	BINDIR:=release
else
	BINDIR:=debug
endif

.PHONY: all clean SYW

all: SYW

OBJECTS=

# Compile C++ source files into object files
%.o:	%.cpp
	@if $(CXX) $(CXXFLAGS) $(INCLUDES) -c $^;\
		then echo "--- Compile \"$^\": Success ---" | $(LOGGER) ;\
		else echo "--- Compile \"$^\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

# Create static library from object files
%.a:	%.o
	@if ar rcs $@ $<;\
		then echo "--- Archive \"$@\": Success ---" | $(LOGGER) ;\
		else echo "--- Archive \"$@\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

SYW:	SYW.o $(LIBXML)
	@mkdir -p $(BINDIR)
	@if $(CXX) -o $@ $^ $(LDLIBS) $(LIBXML);\
		then echo "--- Link \"$@\": Success ---" | $(LOGGER) ;\
		else echo "--- Link \"$@\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

clean:
	@if rm -fv release/* debug/* *.o *.a && rm -rf repo/;\
		then echo "--- \"$@\": Success ---" | $(LOGGER) ;\
		else echo "--- \"$@\": FAILURE! ---" | $(LOGGER) ; exit 1; fi
