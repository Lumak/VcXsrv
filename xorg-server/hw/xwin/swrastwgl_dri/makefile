SHAREDLIB = swrastwgl_dri

INCLUDES += $(OBJDIR) ..\..\.. .. ..\glx $(MHMAKECONF)\gl\include
DEFINES += INSERVER HAVE_XWIN_CONFIG_H PTW32_STATIC_LIB

$(OBJDIR)\swrastwgl_dri.dll : ..\..\..\$(SERVOBJDIR)\vcxsrv.exe
LINKLIBS += ..\..\..\$(SERVOBJDIR)\vcxsrv.lib

CSRCS = swrastwgl_dri.c glwrap.c wgl_ext_api.c

vpath %.c ../glx

$(OBJDIR)\generated_gl_wrappers.c: ../glx/gen_gl_wrappers.py ../glx/gl.xml ../glx/gen_gl_wrappers.py ../glx/reg.py ../../../glx/dispatch.h
	python3.6m.exe ../glx/gen_gl_wrappers.py -outfile $@ -nodbgcount -registry ../glx/gl.xml -staticwrappers -dispatchheader ../../../glx/dispatch.h

$(OBJDIR)\generated_wgl_wrappers.c: ../glx/gen_gl_wrappers.py ../glx/wgl.xml ../glx/gen_gl_wrappers.py ../glx/reg.py
	python3.6m.exe ../glx/gen_gl_wrappers.py -outfile $@ -nodbgcount -registry ../glx/wgl.xml -prefix wgl -preresolve
