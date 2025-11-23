@echo off
echo ========================================
echo    GERANDO DOCUMENTACAO DOXYGEN
echo ========================================

REM Verifica se o Doxygen está instalado
doxygen --version >nul 2>&1
if errorlevel 1 (
    echo ERRO: Doxygen não encontrado!
    echo Instale o Doxygen: https://www.doxygen.nl/download.html
    pause
    exit /b 1
)

REM Limpa documentação anterior
if exist docs rmdir /s /q docs

REM Gera nova documentação
echo Gerando documentacao...
doxygen Doxyfile

if errorlevel 1 (
    echo ERRO: Falha ao gerar documentacao!
    pause
    exit /b 1
)

echo.
echo ========================================
echo    DOCUMENTACAO GERADA COM SUCESSO!
echo ========================================
echo.
echo Arquivos gerados em: docs/html/
echo Abra: docs/html/index.html
echo.

REM Abre no navegador
start docs/html/index.html

pause