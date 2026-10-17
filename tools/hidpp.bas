REM >hidpp - send one HID++ request to a Logitech receiver, or watch it
REM Diagnostic tool for LogiFn. (C) 2026 David Sharp, GPL v3 or later
REM https://github.com/davidpsharp
REM Built 100% by Claude, Anthropic's AI (Claude Code).
REM Unload LogiFn first (*RMKill LogiFn): only
REM one program can have the receiver's HID++ stream open.
REM
REM   BASIC -quit hidpp USB4 req 10 FF 81 02 00 00 00   (7 hex bytes)
REM   BASIC -quit hidpp USB4 watch                      (90 s of notifications)
REM
REM Useful requests (device index 01 = first paired device):
REM   10 FF 81 02 00 00 00   receiver: number of connected devices
REM   10 FF 83 B5 20 00 00   receiver: pairing info for device 1 (wpid at bytes 7-8)
REM   10 01 00 0A 40 A2 00   device 1: index of feature 40A2 (Fn inversion)
REM   10 01 09 0A 00 00 00   device 1: read Fn state, if the feature is at index 9
REM   10 01 09 1A 00 00 00   device 1: set F-keys (1A 01 for media keys)
ON ERROR PRINT REPORT$;" at line ";ERL:IF in% THEN CLOSE#in%:END ELSE END
DIM req% 8, nm% 32
in%=0
a$=FNarg:dev$=LEFT$(a$,INSTR(a$+" "," ")-1):rest$=MID$(a$,LEN(dev$)+2)
IF dev$="" THEN PRINT "Syntax: hidpp USB<n> req <7 hex bytes> | watch":END
$nm%=dev$+CHR$0
in%=OPENIN("devices#interface2;endpoint3;interrupt:"+dev$)
IF in%=0 THEN PRINT "Can't open ";dev$:END
CASE LEFT$(rest$,4) OF
WHEN "req ":
  r$=MID$(rest$,5)+" "
  FOR i%=0 TO 6:p%=INSTR(r$," "):req%?i%=EVAL("&"+LEFT$(r$,p%-1)):r$=MID$(r$,p%+1):NEXT
  PROCsend
  PROClisten(100)
WHEN "watc":
  REM turn on the receiver's wireless notifications (register 0x00, r1 bit 0)
  ?req%=&10:req%?1=&FF:req%?2=&80:req%?3=0:req%?4=0:req%?5=1:req%?6=0
  PROCsend
  PRINT "Watching for 90 s"
  PROClisten(9000)
OTHERWISE:
  PRINT "Syntax: hidpp USB<n> req <7 hex bytes> | watch"
ENDCASE
CLOSE#in%:in%=0
END

DEF FNarg
LOCAL a$,p%
SYS "OS_GetEnv" TO a$
p%=INSTR(a$," -quit ")
IF p% THEN a$=MID$(a$,p%+7):p%=INSTR(a$," "):IF p% THEN =MID$(a$,p%+1)
=""

REM SET_REPORT: class request to the interface, output report 0x10, 7 bytes
DEF PROCsend
LOCAL r0%,fl%,s$,i%
SYS "XDeviceFS_CallDevice",&80000000,nm%,,&21+(&09<<8)+(&0210<<16),2+(7<<16),req%,0 TO r0%;fl%
IF fl% AND 1 THEN s$="":i%=r0%+4:WHILE ?i%:s$+=CHR$?i%:i%+=1:ENDWHILE:PRINT "Send failed: ";s$
ENDPROC

REM Only reads bytes already in the buffer (EXT#), so BGET never waits
DEF PROClisten(cs%)
LOCAL t0%,n%,i%,s$
t0%=TIME
REPEAT
  n%=EXT#in%
  IF n% THEN
    s$="":FOR i%=1 TO n%:s$+=RIGHT$("0"+STR$~BGET#in%,2)+" ":NEXT
    PRINT STR$((TIME-t0%)/100);"s: ";s$
  ENDIF
UNTIL TIME-t0%>cs%
ENDPROC
