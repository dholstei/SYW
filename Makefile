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
# Qt 6.5.3 (installer layout): explicit flags, no pkg-config
QT_PREFIX ?= /usr/local/Qt/6.5.3/gcc_64
QT_INC    := $(QT_PREFIX)/include
QT_LIB    := $(QT_PREFIX)/lib
MOC       := $(QT_PREFIX)/libexec/moc


INCLUDES:=-I/usr/include/libxml2 -I~/Repos/XmlCls
INCLUDES:=$(INCLUDES) -I/usr/include
INCLUDES:=$(INCLUDES) -I./ -I../XmlCls -I../cpp-base64
INCLUDES:=$(INCLUDES) -I$(QT_INC) -I$(QT_INC)/QtCore -I$(QT_INC)/QtGui -I$(QT_INC)/QtWidgets
LDFLAGS=$(DEBUG)
LDLIBS:=-lpthread -lxml2 -L$(QT_LIB) -lQt6Widgets -lQt6Gui -lQt6Core -Wl,-rpath,$(QT_LIB)
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
	@if $(CXX) $(CXXFLAGS) $(INCLUDES) -c $<;\
		then echo "--- Compile \"$<\": Success ---" | $(LOGGER) ;\
		else echo "--- Compile \"$<\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

# Run Qt's meta-object compiler on headers containing Q_OBJECT
moc_%.cpp:	%.h
	@if $(MOC) $< -o $@;\
		then echo "--- Moc \"$<\": Success ---" | $(LOGGER) ;\
		else echo "--- Moc \"$<\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

# Header dependencies
SYW.o MainWindow.o: SYW.h MainWindow.h

# Create static library from object files
%.a:	%.o
	@if ar rcs $@ $<;\
		then echo "--- Archive \"$@\": Success ---" | $(LOGGER) ;\
		else echo "--- Archive \"$@\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

SYW:	SYW.o MainWindow.o moc_MainWindow.o $(LIBXML)
	@mkdir -p $(BINDIR)
	@if $(CXX) -o $@ $^ $(LDLIBS) $(LIBXML);\
		then echo "--- Link \"$@\": Success ---" | $(LOGGER) ;\
		else echo "--- Link \"$@\": FAILURE! ---" | $(LOGGER) ; exit 1; fi

clean:
	@if rm -fv release/* debug/* *.o *.a moc_*.cpp && rm -rf repo/;\
		then echo "--- \"$@\": Success ---" | $(LOGGER) ;\
		else echo "--- \"$@\": FAILURE! ---" | $(LOGGER) ; exit 1; fi