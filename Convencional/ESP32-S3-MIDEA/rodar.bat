@echo off
chcp 65001 > nul
cd /d "%~dp0"

REM --- Acha o Python: tenta o launcher 'py', depois 'python', depois caminho fixo ---
set PY=
where py >nul 2>nul && set PY=py
if "%PY%"=="" ( where python >nul 2>nul && set PY=python )
if "%PY%"=="" if exist "%LOCALAPPDATA%\Programs\Python\Python314\python.exe" set PY="%LOCALAPPDATA%\Programs\Python\Python314\python.exe"

if "%PY%"=="" (
  echo Python nao encontrado. Instale em https://www.python.org/downloads/
  echo e marque "Add Python to PATH" durante a instalacao.
  pause
  exit /b 1
)

echo Usando Python: %PY%
echo Verificando dependencias...
%PY% -m pip install -q -r requirements.txt

%PY% get_device_info.py
pause
