# MuFlex V14 — captura Stage0 usando MSBuild (VS2026)

Esta alteração usa o código-fonte do Unthemidator original e adiciona instrumentação **opcional**. Não execute o GameServer reconstruído: o Stage0 ainda é não validado. O modo `--no-startup` não executa a inicialização do jogo, mas **emula** a rotina de descompressão contida no arquivo.

## 1. Baixar a branch na VM

Em uma cópia limpa:

```powershell
git clone --branch fix/muflex-v14-stage0-capture-msbuild --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git C:\S21Lab\unthemidator-v14
cd C:\S21Lab\unthemidator-v14
```

Se já clonou a branch, execute `git submodule update --init --recursive`.

## 2. Compilar no Visual Studio 2026

```powershell
& ".\scripts\build-v14-vs2026.ps1"
```

O script verifica as dependências, reutiliza `build/*.sln*` ou gera a solução quando necessário e chama o **MSBuild.exe** diretamente. O log de falha estará em `build\muflex_v14_msbuild.log`. Se o PowerShell bloquear scripts, abra uma sessão isolada com `powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-v14-vs2026.ps1`.

## 3. Executar apenas a captura emulada, na VM

```powershell
$Repo = "C:\S21Lab\unthemidator-v14"
$Exe = "$Repo\build\Release\unthemidator.exe"
$Output = "C:\S21Lab\resultados_v14"
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$env:MUFLEX_STAGE0_DIR = $Output

# O catálogo do Unthemidator deve estar no diretório atual.
Push-Location (Split-Path $Exe -Parent)
try {
    & $Exe --no-startup --out-file "$Output\GameServer.STAGE0_DO_NOT_RUN.exe" "C:\S21Lab\GameServer(5).exe" *>&1 |
        Tee-Object "$Output\unthemidator_stdout.log"
    Write-Host "Exit code: $LASTEXITCODE"
} finally {
    Pop-Location
    Remove-Item Env:\MUFLEX_STAGE0_DIR -ErrorAction SilentlyContinue
}
```

Arquivos esperados:
* `GameServer.prelude.before.bin`, `GameServer.prelude.after.bin` — 8192 bytes cada, capturados em duas páginas independentes.
* `GameServer.prelude.before.txt` e `GameServer.prelude.after.txt` — indicam, por página, se o Unicorn conseguiu lê-la. **Zero por erro não representa dados válidos**.
* `GameServer.prelude.LZ_READS.csv` — somente acessos que se sobrepõem ao endereço `image_base + 0xA34F000 - 165`, máximo 256 eventos.
* Em falha de descompressão, `GameServer.prelude.failure.bin/.txt` substitui a captura `after`.

Se os arquivos `GameServer.prelude.*` não aparecerem, verifique se o EXE novo foi compilado da branch, se a variável `MUFLEX_STAGE0_DIR` está definida e se o alvo possui o layout reconhecido (AMD64; EP RVA `0xAF07058`; seção `.themida` RVA `0xA34F000`).

**Observação:** esta instrumentação não restaura automaticamente imports, fórmulas ou handlers. Também não valida o Stage0 como um GameServer executável. O fluxo foi desenvolvido para o binário fornecido e deve ser usado apenas em VM controlada.

## Testes

A workflow `.github/workflows/build-windows-msbuild.yml` compila Release x64 e executa somente `unthemidator.exe --help`, nunca os executáveis protegidos. O runner Windows 2025/VS2026 do CI e o script local utilizam Visual Studio 2026; a compilação é realizada pelo MSBuild.
