# MuFlex V25 — Captura de candidatos a nomes exportados

## Evidências V24 confirmadas diretamente no arquivo Resultados_V24.zip

- ZIP íntegro com 25 arquivos e teste de CRC sem falhas.
- GameServer original SHA-256: `ec083496f2f51a7d3f0d4f7849bf60bf1b7777a68c8ce1f869b8038dba6e7884`.
- 2.000.000 de instruções Unicorn, todas na `.themida` do GameServer. Limite intencional alcançado.
- Os dois JNE tiveram seus seis bytes verificados e o monitor foi habilitado. Nenhuma divergência flags/decisões.
- `dec_DH_JNE` em `0x14A396DD2`: **2.112** avaliações (1.848 tomadas + 264 fallthrough). Todas as sequências no CSV, em ordem, têm `DH=7,6,5,4,3,2,1,0`, repetidas **264 vezes** sem inconsistência. Hipótese V23 de 8 avaliações comprovada.
- `cmp_AL_RDI_JNE` em `0x14A3B0FC1`: **1.018** avaliações; 998 tomadas (byte diferente) e 20 fallthrough (byte igual). 1.018/1.018 leituras `[RDI]` acessíveis; 0 divergências CMP/ZF/fluxo.
- `AL=76`, ASCII `L`, em todas as 1.018 comparações. Valores `[RDI]` são letras ASCII A-I, K, L, sem J observado, e em ordem crescente. Os 20 matches correspondem aos 20 últimos acessos.
- Os endereços `[RDI]` são **estritamente crescentes** em toda a captura, de `0x14B67C27F` até `0x14B681716`, dentro da região da DLL `kernel32.dll` simulada carregada em `0x14B676000`.
- O repositório Unthemidator contém `c_imports::load_module()` que cria DLL fictícia a partir de um catálogo de exports quando o DLL local não está disponível. Portanto o padrão sugere **varredura de nomes exportados no módulo emulado**, e não recuperação de habilidade MU, mas a V24 não capturou nomes completos.
- Contadores V18–V23 mantidos exatamente: 69.550 escritas PE, 60.998 na página hotspot (88 bytes diferentes ao final), 100.320 escritas externas, 56.726 PCs, 106.090 transições não sequenciais.

## V25: ler os nomes próximos do byte examinado

A V25 adiciona o observador read-only `src/helpers/muflex_runtime_export_name_probe.hpp`, acionado somente na comparação já validada `CMP AL,[RDI]` do GameServer x64. Ele lê, com `uc_mem_read`, até 96 bytes ASCII imprimíveis iniciando em RDI e interrompe em NUL, byte não imprimível ou falha de leitura. Não modifica memória, registradores, estado de flags ou o fluxo da execução emulada.

- `runtime_export_candidate_names.csv`: até 2048 registros da comparação, contendo índice da instrução, endereço `[RDI]`, caracteres comparados, igualdade, nome ASCII escapado e motivo de parada.
- `runtime_export_candidate_summary.txt`: total de leituras, acertos, status de captura, RDI não crescente, limite excedido.
- Para evitar ambiguidade com formatos CSV, pontuação é representada por `%HH`; strings lidas são dados não confiáveis, nunca executados.
- Não assume que o ponteiro é o começo do nome exportado: os resultados serão tratados como **candidatos** e comparados com o layout do catálogo/DLL fictícia na próxima análise.

## Windows/Visual Studio 2026 — build e captura

~~~powershell
cd C:\S21Lab
git clone --branch feat/muflex-v25-export-name-trace --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git unthemidator-v25
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v25\scripts\build-v14-vs2026.ps1"
~~~

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v25\scripts\run-v25-probe.ps1" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v25_gameserver" -Budget 2000000 -TimeoutSeconds 600
~~~

~~~powershell
Compress-Archive -Path "C:\S21Lab\resultados_v25_gameserver\*" -DestinationPath "C:\S21Lab\Resultados_V25.zip" -Force
~~~

**Não executar o GameServer Stage0 reconstruído como programa.** O teste apenas emula o GameServer original e sai sem gerar executável reconstruído.

## Próximo critério

Se os candidatos começarem com A, B, C...L e os nomes das 20 igualdades começarem com `L`, comparar com a lista `kernel32.dll` do catálogo e confirmar se o runtime procura uma função específica. Isso delimita o gargalo da inicialização, sem confundir varredura de exports com código original do GameServer. OEP e handlers permanecem não recuperados.
