# MuFlex V21 — investigar escritas guest fora da imagem PE original

## Evidências confirmadas da V20

- Entrada: GameServer(5).exe original, SHA256 EC083496F2F51A7D3F0D4F7849BF60BF1B7777A68C8CE1F869B8038DBA6E7884.
- O runtime emulado parou após 2.000.000 instruções, todas observadas na seção .themida.
- 69.550 escritas guest reportadas no range do PE, 116 páginas, sendo 60.998 na página RVA 0xA392000.
- A página de 4096 bytes foi lida antes/depois com uc_status=0; apenas 88 bytes eram diferentes ao final.
- O snapshot ANTES do runtime é idêntico à página do GameServer Stage0 já recuperado: 4096/4096 bytes.
- 60.998 escritas concentraram-se em 46 endereços de destino, distribuídas por 1.498 PCs escritores e 1.816 combinações (PC,destino,largura).
- Destinos mais ativos: A3921E8 (15193), A3922D8 (10384), A3922F8 (5901), A39220E (4606), A39235B (3187).
- Os locais mais escritos são inicialmente zerados e acumulam valores e ponteiros. Isso é COMPATÍVEL com estado temporário do runtime ofuscado, mas não é prova definitiva do papel de cada campo.
- O estágio original não tem OEP nativo confirmado nem imports reconstruídos.

## Por que a V21

A V19/V20 instalava o hook UC_HOOK_MEM_WRITE somente na imagem PE. Isso deixava sem observação escritas geradas por instruções do runtime em região externa, incluindo possíveis heaps, stacks e módulos simulados. A V21 estende o mesmo hook para todos os endereços acessíveis ao Unicorn e separa as escritas internas das externas. Portanto, não é necessário aumentar o orçamento de 2 milhões de instruções para comparar os dois conjuntos de resultados.

## Arquivos novos

- runtime_external_pages.csv: ranking de páginas de 4096 bytes fora da imagem PE, número de eventos, primeiro/último PC escritor, primeiro/último índice e permissões r/w/x QUANDO a emulação terminou.
- runtime_external_samples.csv: primeiras 128 escritas externas e amostras periódicas, até 2048 linhas.
- runtime_external_summary.txt: quantidade de escritas externas, páginas externas monitoradas (teto 8192), eventos sem classificação por limite, páginas com permissão de execução no FINAL da emulação.
- runtime_summary.txt: campos external_guest_writes e external_tracked_pages. Os resultados e arquivos V20 permanecem intactos.

**Limites de interpretação**

- Uma página que tem permissão executável ao final NÃO necessariamente era executável quando a escrita ocorreu.
- A heurística near_rsp_first_1mb significa só que a página estava a até 1 MiB do valor RSP registrado no primeiro evento; não é prova de que ela seja a stack.
- O hook vê escritas guest emitidas pelo Unicorn, não as operações host uc_mem_write chamadas pelo emulador.
- Não executa GameServer nativamente e não constrói um PE final no modo de diagnóstico.
- Grande volume de escritas externas pode aumentar significativamente o custo da emulação. O contador limita a memória consumida, mas use o prazo externo de 600 segundos.

## Como testar na VM Windows com Visual Studio 2026

Clone e compile em diretório novo:

~~~powershell
cd C:\S21Lab
git clone --branch feat/muflex-v21-external-memory-probe --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git unthemidator-v21
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v21\scripts\build-v14-vs2026.ps1"
~~~

Emule apenas com a instrumentação, sem iniciar GameServer:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v21\scripts\run-v21-probe.ps1" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v21_gameserver" -Budget 2000000 -TimeoutSeconds 600
~~~

Depois envie somente diagnósticos:

~~~powershell
Compress-Archive -Path "C:\S21Lab\resultados_v21_gameserver\*" -DestinationPath "C:\S21Lab\Resultados_V21.zip" -Force
~~~

## Critérios de avanço

1. Verificar se o número de escritas internas permanece ~69.550 e hotspot ~60.998: isso indica que o hook global não alterou materialmente a trajetória observada.
2. Classificar páginas externas por frequência, permissão, endereço e proximidade do RSP. Escritas externas por si só não são prova de código recuperado.
3. Se houver páginas externas que estejam executáveis ao final, inspecionar bytes, contexto e temporização antes de declarar qualquer rotina nativa.
4. Se predominarem stack e dados temporários, voltar aos pontos de controle VM/dispatch e à infraestrutura de importação, sem presumir OEP.

Não executar PE Stage0 como programa.
