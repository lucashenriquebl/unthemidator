# MuFlex V24 — validar duas condições JNE do runtime GameServer

## Motivação: V23 mediu o fluxo completo

- `Resultados_V23.zip` foi validado (22 arquivos, CRC íntegros).
- 2.000.000 instruções na `.themida`, **106.090** transições não sequenciais em **2.044** pares; 56.726 PCs distintos.
- As janelas 9, 10 e 11 concentraram **79.508 transições** (74,94% do total). Janela 10: 35.167 transições entre apenas 141 pares diferentes.
- Disassembly do Stage0 x64 confirma `DEC DH` em `0x14A66CC37`, `JMP` em `0x14A66CC39` e `JNE` em `0x14A396DD2`.
- A aresta para o JNE apareceu 2.112 vezes, enquanto o ramo tomado apareceu 1.848 vezes. Diferença de 264, compatível (mas ainda não comprovada) com ciclos de oito avaliações.
- Outro bloco executa `CMP AL,[RDI]` em `0x14A4078D0` e `JNE` em `0x14A3B0FC1`. Há 1.018 entradas no bloco e 998 desvios tomados.

## Instrumentação passiva V24

`src/helpers/muflex_runtime_branch_probe.hpp` observa apenas o GameServer x64 esperado (RVA original de entrada `0xAF07058`), e verifica os **6 bytes exatos** de ambos os JNEs na memória do Unicorn após o Stage0. Se os bytes não coincidirem, desativa o monitor específico sem alterar a emulação.

Durante a execução, no hook `UC_HOOK_CODE`, registra o RFLAGS, `DH`, `AL`, `RDI`, `RAX`, `RDX`, `RBX`, `RSP`, e lê somente um byte em `[RDI]` quando acessível. Quando a próxima instrução executar, classifica a transição como desvio tomado, fallthrough ou inesperada. Não modifica registradores, flags, páginas ou o fluxo do Unicorn.

- `runtime_branch_decisions.csv`: **todas** as observações de branch até o teto de 16.384 linhas, com valores anteriores ao branch e próximo PC efetivo.
- `runtime_branch_dh_histogram.csv`: distribuição de `DH` por decisão, para validar hipótese de contador de oito iterações.
- `runtime_branch_summary.txt`: quantidades de tomadas, fallthrough e divergências de interpretação entre ZF e o fluxo, além de leituras `[RDI]` falhas.
- `runtime_summary.txt`: informa se o monitor específico está habilitado; mantém as métricas V18–V23.

Este observador não recupera por si só o OEP do jogo, nem funções de combate. A análise de flags faz sentido apenas se a sequência de instruções observada corresponde ao código esperado. Comparação de bytes é feita antes do hook iniciar.

## Execução na VM Windows com Visual Studio 2026

~~~powershell
cd C:\S21Lab
git clone --branch feat/muflex-v24-conditional-branches --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git unthemidator-v24
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v24\scripts\build-v14-vs2026.ps1"
~~~

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v24\scripts\run-v24-probe.ps1" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v24_gameserver" -Budget 2000000 -TimeoutSeconds 600
~~~

~~~powershell
Compress-Archive -Path "C:\S21Lab\resultados_v24_gameserver\*" -DestinationPath "C:\S21Lab\Resultados_V24.zip" -Force
~~~

## Critérios para interpretar resultados

1. Confirmar que a rotina `dec_DH_JNE` foi visitada 2.112 vezes, com aproximadamente 1.848 decisões tomadas e 264 fallthrough; histogramas de DH devem mostrar com que valor a condição termina.
2. Confirmar que `cmp_AL_RDI_JNE` foi visitada cerca de 1.018 vezes, com aproximadamente 998 tomadas e 20 fallthrough; verificar `ZF` e se `AL == byte[RDI]` antes do JNE.
3. Se o monitor indicar salto inesperado, corrupção de flags ou código diferente do Stage0, investigar compatibilidade do hook — não afirmar recuperação nativa.
4. Conferir que as contagens de 2 milhões de instruções, 69.550 escritas internas e 100.320 externas permanecem estáveis.

**Somente emulação Unicorn do arquivo original. Não executar nenhum Stage0 reconstruído como programa. PR deve permanecer draft até CI e captura da VM.**
