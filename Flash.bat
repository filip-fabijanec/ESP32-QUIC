@echo off
setlocal
cd /d "%~dp0"

:: ---------------------------------------------------
:: KONFIGURACIJA
:: ---------------------------------------------------
set COM_PORT=COM6
set BAUD_RATE=460800
set MONITOR_BAUD=115200
set PROJECT_NAME=quic_demo.bin
:: ---------------------------------------------------

echo.
echo ===================================================
echo  PRIPREMA ZA FLASHANJE (%PROJECT_NAME%)
echo ===================================================

:: Provjera datoteka
if not exist "build\%PROJECT_NAME%" (
    echo [GRESKA] Nema build\%PROJECT_NAME%. Kopiraj build folder iz Dockera!
    pause
    exit /b
)

echo.
echo ===================================================
echo  POCINJEM FLASHANJE NA %COM_PORT%
echo ===================================================

:: Flashanje (ignoriramo upozorenja, bitno je da esptool odradi posao)
python -m esptool --chip esp32s3 -b %BAUD_RATE% --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m 0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0xd000 build/ota_data_initial.bin 0x10000 build/%PROJECT_NAME%

:: Ovdje provjeravamo ERRORLEVEL. Ako je esptool stvarno pao (npr. nema kabela), value ce biti >= 1.
if %ERRORLEVEL% GEQ 2 (
    echo.
    echo [GRESKA] Flashanje nije uspjelo! Provjeri kabel i BOOT tipku.
    pause
    exit /b
)

echo.
echo ===================================================
echo  FLASHANJE GOTOVO!
echo ===================================================
echo.
echo Otvaram Serial Monitor za 3 sekunde...
echo (Pritisni CTRL + ] za izlaz iz monitora)
echo.

timeout /t 3 >nul

:: Pokretanje monitora
python -m serial.tools.miniterm "%COM_PORT%" %MONITOR_BAUD%

:: Ako monitor pukne, da vidimo gresku
pause