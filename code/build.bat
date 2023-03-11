@echo off

pushd ..\data
del atlas.png
del atlasMetadata.txt
popd

IF NOT EXIST ..\build mkdir ..\build
pushd ..\build
del /Q/F/S *.* > nul
cl -MT -nologo -Gm- -GR- -EHsc- -Od -Oi -Zi -FC -W4 -Qpar -wd4127 -wd4706 -wd4100 -wd4996 -wd4505 ../code/main.cpp /link -incremental:no -opt:ref user32.lib winmm.lib gdi32.lib /out:texpack.exe
set last_error=%ERRORLEVEL%
set start_prg=0

IF NOT %last_error%==0 GOTO :end
IF NOT %start_prg%==1 GOTO :end

call texpack.exe ..\data
pushd ..\data
start atlas.png

:end
popd
popd

