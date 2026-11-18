# MuFlex V20 — observar a página de maior atividade no runtime GameServer

## Resultados confirmados V19

- GameServer original x64 SHA256: EC083496F2F51A7D3F0D4F7849BF60BF1B7777A68C8CE1F869B8038DBA6E7884.
- No Unicorn: 2.000.000 instruções, todas em .themida; limite alcançado.
- 69.550 escritas guest na imagem PE, todas na .themida; 116 páginas distintas.
- Página RVA 0xA392000: 60.998 escritas (87,70% do total).
- As cinco páginas mais modificadas somam 97,79% das escritas.
- A V19 gravou 304 amostras; por isso não era suficiente para determinar as instruções responsáveis por cada uma das 60.998 escritas na página mais ativa.
- Não existe ainda OEP nativo validado, imports reconstituídos, nem função de habilidade recuperada.

## O que a V20 acrescenta

Apenas para o layout do GameServer x64 esperado, o modo --runtime-probe-dir:

1. Lê os 4.096 bytes da página RVA 0xA392000 imediatamente antes de entrar em startup().
2. Durante o startup emulado, agrega cada acesso de escrita dessa página por (PC escritor, VA do destino, largura da escrita). Cada chave registra quantidade, primeira/última instrução e primeiro/último valor reportado por Unicorn.
3. Salva a mesma página no final da emulação, mesmo se o método startup() lançar uma exceção após atingir o orçamento.
4. Gera runtime_hotspot_writers.csv, runtime_hotspot_before.bin, runtime_hotspot_after.bin e runtime_hotspot_summary.txt. Os snapshots binários são criados apenas quando uc_mem_read teve sucesso; o summary informa os erros de leitura e a contagem de bytes diferentes.
5. Amplia o runtime_summary.txt com hotspot_guest_writes, hotspot_distinct_keys, hotspot_dropped_key_events e hotspot_changed_bytes.

O acumulador guarda até 8.192 chaves distintas. Eventos adicionais que ultrapassem esse limite continuam contabilizados, mas não recebem classificação por chave. Não há alteração na memória guest pela instrumentação; o diagnóstico ainda não gera PE reconstruído nem inicia o GameServer nativamente.

## Teste na VM Windows / Visual Studio 2026

Clone e compile em um diretório separado:

~~~powershell
cd C:\S21Lab
git clone --branch feat/muflex-v20-hotspot-page-trace --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git unthemidator-v20
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v20\scripts\build-v14-vs2026.ps1"
~~~

Execute a observação limitada, usando o GameServer original:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v20\scripts\run-v20-probe.ps1" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v20_gameserver" -Budget 2000000 -TimeoutSeconds 600
~~~

Compacte apenas a pasta de diagnósticos:

~~~powershell
Compress-Archive -Path "C:\S21Lab\resultados_v20_gameserver\*" -DestinationPath "C:\S21Lab\Resultados_V20.zip" -Force
~~~

## Como interpretar

- Se hotspot_guest_writes estiver próximo de 60.998, a instrumentação nova reproduziu a atividade da captura V19.
- Muitas escritas mas poucos bytes alterados no snapshot significam atualizações transitórias: uma observação mais compatível com estado dinâmico do runtime do que restauração permanente, mas não comprova função específica.
- Muitos bytes permanentemente modificados pedem desassemblagem e exame do contexto, não permitem afirmar que são handlers do jogo.
- O hook observa somente escritas guest emuladas reportadas por Unicorn; operações host uc_mem_write e alocações externas permanecem fora da cobertura desta captura específica.
- O teto de 2 milhões é voluntário. Mensagens de falta de startup entry calls após o teto não provam falha de descompactação.

**Nunca executar como programa um executável Stage0 criado pelo Unthemidator.**
