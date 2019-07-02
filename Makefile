# IBM C/C++ (VisualAge) Makefile for ipecho
#

CC     = icc.exe
RC     = rc.exe
LINK   = ilink.exe
CFLAGS = /Ss /O /Gl /Q+ /Wuse /Gm /Gd
RFLAGS = -n
LFLAGS = /PMTYPE:VIO /NOLOGO /MAP
NAME   = ipecho
OBJS   = ipecho.obj

BL_VER  = "1.0"
BL_VEND = "Alex Taylor"
BL_DESC = "Public IP address (echo) reporting tool"

!ifdef DEBUG
    CFLAGS   = $(CFLAGS) /Ti /Tm
    LFLAGS   = $(LFLAGS) /DEBUG
!endif


$(NAME).exe : $(OBJS) Makefile
                @call makedesc -D$(BL_DESC) -N$(BL_VEND) -V$(BL_VER) $(NAME).def
                $(LINK) $(LFLAGS) $(OBJS) $(NAME).def /OUT:$@
                @dllrname.exe $@ CPPOM30=OS2OM30 /Q /R

clean       :
              -del $(OBJS) $(NAME).exe $(NAME).map 2>NUL


