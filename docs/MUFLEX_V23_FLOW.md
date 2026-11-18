# MuFlex V23 — observar as transições repetidas no runtime

## O que foi confirmado no teste V22

- ZIP `Resultados_V22.zip` validado (18 arquivos, CRC íntegros).
- GameServer SHA-256: `ec083496f2f51a7d3f0d4f7849bf60bf1b7777a68c8ce1f869b8038dba6e7884`.
- `instructions_observed=2000000`, `coverage_distinct_pc_keys=56726`, `coverage_windows=20`, `coverage_untracked_global_pc_hits=0`.
- **Todas as instruções observadas ainda na `.themida`**; `startup_returned=false`, `stop_reason=instruction_budget_reached`, 0 imports, 0 trampolins.
- Nas primeiras 1.200.000 instruções apareceram 55.176 PCs distintos. Nas últimas 800.000 instruções, apenas **1.550 novos**.
- Janela 10 (900.001–1.000.000): **399 PCs distintos naquela janela**, com 274 globalmente inéditos. Foi a janela com maior concentração de repetição local.
- Janela 13 (1.200.001–1.300.000): 16.413 PCs distintos naquela janela, **zero** globalmente inéditos.
- Últimas 4 janelas: 773 PCs globalmente inéditos em 400.000 execuções.
- 512 PCs mais executados respondem por 412.817 execuções (20,64%); o ranking de PCs não permite estabelecer sozinho um único loop principal.
- Reproduziu os contadores V21: 69.550 escritas no PE, 60.998 no hotspot, 88 bytes diferentes nos snapshots, 100.320 escritas externas em apenas uma página.

## Novo instrumento V23

O hook `UC_HOOK_CODE` continua observacional. O novo `muflex_runtime_flow.hpp` registra a transição de um PC anterior para o PC atual **somente quando o PC atual difere do endereço seguinte esperado pela instrução anterior**. Isso registra uma descontinuidade de fluxo observada, não necessariamente uma instrução de salto: callbacks, redirecionamentos internos do Unicorn e pausas/resumes também podem causar diferenças.

- `runtime_flow_windows.csv`: a cada 100.000 instruções, quantidade de PCs consecutivos, descontinuidades, arestas diferentes, arestas inéditas globalmente e reutilização.
- `runtime_flow_window_top_edges.csv`: até 24 pares RIP-origem/RIP-destino mais frequentes em cada janela.
- `runtime_flow_global_top_edges.csv`: até 256 pares mais frequentes no teste todo.
- `runtime_flow_summary.txt`: cobertura de arestas, totais e limites excedidos.
- Limites explícitos: no máximo 131.072 arestas globais e 32.768 por janela. Eventos que excederem o limite são contados como não classificados, sem consumir memória ilimitada.
- A V23 mantém todos os diagnósticos V18–V22 e não modifica a memória guest nem reconstrói PE em modo probe.

## Como compilar e executar na VM Windows/VS2026

~~~powershell
cd C:\S21Lab
git clone --branch feat/muflex-v23-runtime-flow-edges --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git unthemidator-v23
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v23\scripts\build-v14-vs2026.ps1"
~~~

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v23\scripts\run-v23-probe.ps1" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v23_gameserver" -Budget 2000000 -TimeoutSeconds 600
~~~

~~~powershell
Compress-Archive -Path "C:\S21Lab\resultados_v23_gameserver\*" -DestinationPath "C:\S21Lab\Resultados_V23.zip" -Force
~~~

## Interpretação

1. Se a janela 10 mostrar um pequeno conjunto de arestas de alta frequência, reconstruir **somente o grafo local** e entender a condição de saída; não presumir loop infinito.
2. Se as janelas 13–20 revisitarem as mesmas arestas, avaliar eventual recorrência do dispatcher/VM protegido.
3. Se surgirem arestas novas apesar dos poucos PCs inéditos, a máquina pode estar reordenando transições entre blocos já conhecidos.
4. Se o total de arestas atingir os tetos, usar os contadores de saturação e não fazer inferência sobre cobertura exata além deles.
5. Nenhuma das descobertas prova que o OEP foi alcançado nem permite recuperar fórmulas/habilidades da Season 21 sem validação adicional.

**Não executar os arquivos Stage0 como programas. PR draft até testes na VM.**
