# MuFlex V18 — observar o runtime após a descompressão Stage0

**Estado real:** os 12.288.000 bytes de GameServer, 41.984.000 bytes de Main.exe e 6.144.000 bytes de Main.dll foram reproduzidos pelo descompressor offline e conferidos byte a byte com snapshots independentes do Unicorn na VM. **Isso não confirma OEP nativo, imports reconstruídos, handlers de habilidades ou recuperação integral do Themida.**

## O que este modo faz

O Unthemidator emula a descompressão Stage0 como antes e, com a opção especial **`--runtime-probe-dir`**, observa somente a função `startup()` **dentro do Unicorn**. Um hook `UC_HOOK_CODE` registra:
- `runtime_trace.csv`: primeiras 256 instruções, transições entre seções PE, amostras a cada 4096 instruções e última instrução observada. Endereço virtual, RVA, nome da seção, tamanho e bytes de opcode.
- `runtime_summary.txt`: contagem total, quantidade de instruções por seção, se o teto foi alcançado, último PC, possível RVA de entrada (expressamente **não validado como OEP**), número de imports/trampolins recuperados e eventual erro.
- `runtime_stdout.log` e `runtime_stderr.log` via script.
- Nenhum executável reconstruído é criado em modo probe.

O teto padrão é **250.000 instruções**, ajustável entre 1.000 e 2.000.000. O script de VM impõe também **180 segundos de duração total**, matando somente o processo do Unthemidator quando o limite é alcançado. É uma observação limitada, não garantia de que o runtime conseguirá iniciar.

## 1. Clonar e compilar em sua VM Windows (Visual Studio 2026)

```powershell
cd C:\S21Lab
git clone --branch feat/muflex-v18-bounded-runtime-probe --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git unthemidator-v18
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v18\scripts\build-v14-vs2026.ps1"
```

A compilação usa **MSBuild**. Se falhar, envie `C:\S21Lab\unthemidator-v18\build\muflex_v14_msbuild.log`. O nome histórico desse script de compilação foi mantido para evitar quebra de caminhos; ele compila o código atual da branch, incluindo a V18.

## 2. Observação do GameServer

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v18\scripts\run-v18-probe.ps1" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v18_gameserver" -Budget 250000 -TimeoutSeconds 180
```

Se desejar, para Main ou Main.dll, execute o mesmo script com `-InputFile "C:\S21Lab\Main(8).exe"` ou `-InputFile "C:\S21Lab\Main.dll"` e destinos diferentes. Verifique os hashes dos arquivos originais antes da análise.

## 3. O que enviar

Compacte somente a pasta `C:\S21Lab\resultados_v18_gameserver`, incluindo `runtime_trace.csv`, `runtime_summary.txt`, logs e hash SHA. Caso um limite externo ou uma falha do sistema interrompa o processo antes da criação de `runtime_summary.txt`, envie os logs e o CSV parcial, caso exista.

## Como interpretar

**Resultado com instruções executadas apenas na `.themida`:** ainda estamos no runtime protegido. O endpoint da descompressão não é OEP confirmado.

**Resultado com transição para a `.text`:** observação de uma transferência de fluxo, não confirmação de que a função do jogo está desempacotada ou correta. Verificar entropia, bytes, chamadas e contexto.

**Limite de instruções atingido:** interrupção voluntária do observador. `exit_code=2` é esperado nesse caso; não equivale a erro de build.

**Falha antes do hook executar:** usar `runtime_stderr.log` e `runtime_stdout.log` para localizar erro de mapeamento, VM ou análise estática.

**Importante:** não executar o `GameServer.STAGE0_DO_NOT_RUN.exe`, o `Main.Stage0.exe` ou a `MainDLL.Stage0.dll`. O objetivo final continua sendo comparar fórmulas e efeitos da Season 21 com o MuFlex sem substituir a implementação MoMu existente.

## CI

O GitHub Actions compila MSBuild Release x64 e testa a ajuda CLI e a validação de argumentos. Ele não executa nem emula binários protegidos de terceiros. O modo runtime só será validado funcionalmente com os novos registros enviados da VM.
