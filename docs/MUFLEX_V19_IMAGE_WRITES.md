# MuFlex V19 — observar se o runtime restaura código na memória

## Evidência V18 (2 milhões de instruções)

O usuário forneceu Resultados_V18_2M.zip (ZIP íntegro), gerado na VM com GameServer SHA-256 ec083496f2f51a7d3f0d4f7849bf60bf1b7777a68c8ce1f869b8038dba6e7884.

- limit=2000000, instructions_observed=2000000, instruction_limit_reached=true
- instructions_in_.themida=2000000; last_observed_pc=0x14a638f56
- startup_returned=false; error=failed to find startup entry calls
- imports_recovered_so_far=0; vm_trampolines_so_far=0
- runtime_trace.csv: 745 registros, 744 amostras e uma mudança de seção inicial; nenhuma transição para .text
- Módulos Windows emulados: kernel32.dll, user32.dll, advapi32.dll, ntdll.dll, shell32.dll, encaminhados ao dummy
- exit_status.txt ficou sem código (exit_code=); não é o instrumento confiável para diagnosticar o runtime

O limite de instruções interrompeu deliberadamente a execução. A mensagem "failed to find startup entry calls" foi gerada depois, ao verificar que não foram encontradas chamadas de startup. Isso não demonstra que o GameServer jamais possa atingir o código original.

## Instrumentação V19

Com o comando de runtime da V18, a V19 instala mais um hook Unicorn UC_HOOK_MEM_WRITE, restrito à imagem PE, durante a emulação do startup. Captura escritas de instruções emuladas em qualquer seção da imagem:

- runtime_image_writes.csv: até 4096 registros amostrados com RIP e RVA da instrução, endereço destino, seção e página da escrita, tamanho e valor baixo de 64 bits
- runtime_image_pages.csv: páginas de 4096 bytes escritas e número de eventos por página
- runtime_summary.txt: stop_reason e contadores de escrita em cada seção, além dos campos V18
- runtime_trace.csv: observação V18 dos RIPs executados e das transições de seção

Limitações: o hook observa instruções guest que Unicorn reporta como UC_MEM_WRITE; não inclui operações uc_mem_write feitas diretamente pelo host do emulador. Valores de escrita maiores que 8 bytes podem ser truncados. Encontrar escrita em .text NÃO valida a recuperação de funções nativas.

## Como compilar no Windows / Visual Studio 2026

~~~powershell
cd C:\S21Lab
git clone --branch feat/muflex-v19-runtime-image-writes --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git unthemidator-v19
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v19\scripts\build-v14-vs2026.ps1"
~~~

## Executar emulação limitada

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v19\scripts\run-v19-probe.ps1" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v19_gameserver" -Budget 2000000 -TimeoutSeconds 600
~~~

Depois compactar os relatórios:

~~~powershell
Compress-Archive -Path "C:\S21Lab\resultados_v19_gameserver\*" -DestinationPath "C:\S21Lab\Resultados_V19.zip" -Force
~~~

Não é necessário reenviar executáveis Stage0.

## Interpretação

1. Se houver writes_in_.text maior que zero, cruzar os RIPs escritores com RVA das páginas e comparar snapshots antes/depois. Não presumir que toda escrita equivale a recuperação nativa.
2. Se as escritas permanecerem em .themida ou dados, investigar transições e modificações no runtime, não apenas elevar o limite.
3. stop_reason=instruction_budget_reached significa parada voluntária. O erro de falta de startup entry calls posterior pode ser apenas consequência dessa interrupção.

Este modo não inicia o GameServer como processo nem gera um PE reconstruído. Preserve a implementação MuFlex existente; não execute binários Stage0 como programas.
