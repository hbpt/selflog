@echo off
rem ===========================================================================
rem  selftest.bat - regression checks for dist\selflog.exe
rem  ASCII only (cmd.exe parses .bat with the OEM code page).
rem ===========================================================================
setlocal enabledelayedexpansion
set "EXE=%~dp0..\dist\selflog.exe"
cd /d "%~dp0"
set FAIL=0

if not exist "%EXE%" (
    echo [test] %EXE% not found - run build.bat first
    exit /b 1
)

echo --- 1. self mode: args + stdin are recorded ------------------------------
echo hello world> in.txt
type in.txt | "%EXE%" -q --log self.log --truncate --tag selftest --say "out-marker" --err "err-marker" > out.txt 2> err2.txt
findstr /c:"argv[1]       : -q" self.log >nul || (echo [FAIL] argv not logged & set FAIL=1)
findstr /c:"out-marker" out.txt  >nul || (echo [FAIL] --say not on stdout & set FAIL=1)
findstr /c:"err-marker" err2.txt >nul || (echo [FAIL] --err not on stderr & set FAIL=1)
findstr /c:"hello world" self.log >nul || (echo [FAIL] stdin not logged & set FAIL=1)
findstr /c:"stdin=13" self.log >nul || (echo [FAIL] stdin byte count wrong & set FAIL=1)

echo --- 2. explicit wrapper: child output + exit code ------------------------
"%EXE%" -q --log wrap.log --truncate -- cmd /c "echo child-marker & exit 7"
if errorlevel 7 (echo [PASS] exit code 7 propagated) else (echo [FAIL] exit code not propagated & set FAIL=1)
findstr /c:"child-marker" wrap.log >nul || (echo [FAIL] child stdout not logged & set FAIL=1)
findstr /c:"exit=7" wrap.log >nul || (echo [FAIL] exit code not in log & set FAIL=1)

echo --- 3. transparent proxy: shim forwards to [name]_real.exe ---------------
copy /y "%EXE%" "proxydemo.exe" >nul
copy /y "%SystemRoot%\System32\findstr.exe" "proxydemo_real.exe" >nul
set "SELFLOG_FILE=proxy.log"
set "SELFLOG_TRUNCATE=1"
echo hello proxy> pin.txt
type pin.txt | proxydemo.exe "hello" > pout.txt
set "SELFLOG_FILE="
set "SELFLOG_TRUNCATE="
findstr /c:"hello proxy" pout.txt >nul || (echo [FAIL] proxied stdout lost & set FAIL=1)
findstr /c:"mode          : proxy" proxy.log >nul || (echo [FAIL] proxy mode not logged & set FAIL=1)
findstr /c:"real_exe" proxy.log >nul || (echo [FAIL] real exe path not logged & set FAIL=1)
findstr /c:"fwd[1]        : hello" proxy.log >nul || (echo [FAIL] forwarded args not logged & set FAIL=1)

echo --- 4. proxy must NOT swallow the child's own options -------------------
set "SELFLOG_FILE=proxy.log"
type pin.txt | proxydemo.exe "-i" "HELLO" > pout2.txt
set "SELFLOG_FILE="
findstr /c:"hello proxy" pout2.txt >nul || (echo [FAIL] child option not forwarded & set FAIL=1)
findstr /c:"fwd[1]        : -i" proxy.log >nul || (echo [FAIL] option not logged as forwarded & set FAIL=1)

echo --- 5. install / uninstall ----------------------------------------------
copy /y "%SystemRoot%\System32\findstr.exe" "foo.exe" >nul
"%EXE%" --install foo.exe >nul
if not exist "foo_real.exe" (echo [FAIL] install did not rename real exe & set FAIL=1)
type pin.txt | foo.exe "hello" > iout.txt
findstr /c:"hello proxy" iout.txt >nul || (echo [FAIL] installed shim does not forward & set FAIL=1)
"%EXE%" --uninstall foo.exe >nul
if exist "foo_real.exe" (echo [FAIL] uninstall left _real behind & set FAIL=1)
if not exist "foo.exe" (echo [FAIL] uninstall did not restore original & set FAIL=1)
fc /b foo.exe "%SystemRoot%\System32\findstr.exe" >nul || (echo [FAIL] restored file differs from original & set FAIL=1)

echo --- 6. append mode keeps sessions apart ---------------------------------
"%EXE%" -q --log append.log --truncate --no-stdin --tag A
"%EXE%" -q --log append.log --no-stdin --tag B
findstr /c:"tag           : A" append.log >nul || (echo [FAIL] session A missing & set FAIL=1)
findstr /c:"tag           : B" append.log >nul || (echo [FAIL] session B missing & set FAIL=1)

echo --- 7. code page decoding -----------------------------------------------
"%EXE%" -q --cp 936 --log cp.log --truncate --no-stdin
findstr /c:"CP 936" cp.log >nul || (echo [FAIL] cp option not recorded & set FAIL=1)

rem --- cleanup --------------------------------------------------------------
del /q proxydemo.exe proxydemo_real.exe foo.exe pin.txt pout.txt pout2.txt iout.txt in.txt out.txt err2.txt selflog.log proxy.log wrap.log append.log cp.log 2>nul
del /q *.log 2>nul

echo.
if "%FAIL%"=="0" (echo [test] ALL PASS) else (echo [test] SOME CHECKS FAILED)
exit /b %FAIL%
