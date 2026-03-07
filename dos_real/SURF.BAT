@ECHO OFF
REM RetroSurf launcher for real hardware
REM Load packet driver then run browser

REM Check if packet driver already loaded
IF "%MTCP_PACKETINT%"=="0x60" GOTO RUN

ECHO Loading NE2000 packet driver...
ECHO (If this fails, check your IRQ and I/O base address)
NE2000.COM 0x60 10 0x300
IF ERRORLEVEL 1 GOTO DRVFAIL

:RUN
ECHO Starting RetroSurf...
RETRO.EXE
GOTO END

:DRVFAIL
ECHO.
ECHO ERROR: Packet driver failed to load!
ECHO.
ECHO Try one of these (check your card's jumper settings):
ECHO   NE2000.COM 0x60 5 0x300
ECHO   NE2000.COM 0x60 3 0x300
ECHO   NE2000.COM 0x60 10 0x320
ECHO.

:END
