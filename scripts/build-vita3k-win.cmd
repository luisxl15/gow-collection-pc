@echo off
rem Compila o Vita3K (revisão pré-Qt a01f2b15) no Windows com o Build Tools 2022 + Ninja.
rem   scripts\build-vita3k-win.cmd <checkout do Vita3K> [Release|RelWithDebInfo] [jobs]
rem Com VITA3K_RECOMP_RUNTIME definido no ambiente, o patch de integrations\vita3k liga o backend.
setlocal
rem Os .bat do Boost chamam scripts do diretório atual sem caminho; com esta variável o cmd recusa.
set NoDefaultCurrentDirectoryInExePath=
set SRC=%~1
set CFG=%~2
if "%CFG%"=="" set CFG=Release
set JOBS=%~3
if "%JOBS%"=="" set JOBS=3
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%SRC%" || exit /b 1
if not exist build\windows-ninja\build.ninja (
  cmake --preset windows-ninja %VITA3K_CMAKE_ARGS% || exit /b 1
)
cmake --build build\windows-ninja --config %CFG% -j %JOBS% || exit /b 1
echo BUILD_OK
