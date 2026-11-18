# MuFlex V22 — analisar cobertura e repetição do runtime Themida

## Evidência trazida pela V21

- SHA-256 do GameServer original: EC083496F2F51A7D3F0D4F7849BF60BF1B7777A68C8CE1F869B8038DBA6E7884.
- 2.000.000 de instruções observadas, 100% na seção `.themida`. Parada voluntária por instrução máxima.
- Dentro do PE: 69.550 escritas guest, exatamente como na V20; 60.998 na página RVA 0xA392000.
- Fora da imagem PE: **100.320 escritas guest**, TODAS na página 0x13FFEC000, com permissões **rw-** ao final.
- RSP na primeira escrita: 0x13FFECFD8, na mesma página; amostras de escritas 8 bytes, compatíveis com operações de stack. Não há prova de que a página seja stack pelo endereço isoladamente.
- Nenhuma página externa com permissão de execução ao final. A V21 não observa `uc_mem_write` diretamente do host, e não comprova ausência de código externo em outros momentos.
- Snapshot hotspot V21 `before` idêntico ao V20; `after` difere em 1 único byte, offset 0x25F, 0xC6 (V20) versus 0xC9 (V21). Contagens e trace de PCs amostrados foram idênticos; não assumir que todo estado interno é determinístico.
- A ferramenta registra `failed to find startup entry calls` depois de interromper a emulação por limite. Não houve callback confirmado para código nativo; esse erro sozinho não prova falha irreversível.

## Por que esta V22?

O código `c_themida_unpacker::startup()` captura OEP/callbacks por tentativas de execução em regiões protegidas da imagem PE. Como nenhum candidato foi alcançado nas primeiras 2 milhões de instruções, precisamos medir se o PC segue explorando novas instruções Themida ou se passa a repetir um conjunto restrito de endereços.

## Novos relatórios V22

- `runtime_coverage_windows.csv`: uma linha por janela de 100.000 instruções. Contém primeiro/último PC, PCs distintos na janela, visitas a PCs já vistos nela, PCs inéditos no histórico completo e limites de agregação.
- `runtime_coverage_top_pcs.csv`: 512 endereços mais executados e número exato de ocorrências registrados em Unicorn.
- `runtime_coverage_summary.txt`: totais, número de PCs diferentes e indicação de saturação de limites de monitoramento.
- `runtime_summary.txt`: contadores adicionais de cobertura; os diagnósticos V19/V20/V21 permanecem ativos.

Limites: 262.144 PCs globais distintos, 100.000 PCs diferentes por janela. Se um limite for alcançado, os relatórios explicitam quantos hits ficaram sem classificação e não devem ser lidos como contagens exatas de PCs distintos. A monitoração é passiva e não modifica a memória ou o fluxo do guest.

## Instruções para VM / Visual Studio 2026

~~~powershell
cd C:\S21Lab
git clone --branch feat/muflex-v22-runtime-coverage-windows --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git unthemidator-v22
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v22\scripts\build-v14-vs2026.ps1"
~~~

Depois:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "C:\S21Lab\unthemidator-v22\scripts\run-v22-probe.ps1" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v22_gameserver" -Budget 2000000 -TimeoutSeconds 600
~~~

E compacte:

~~~powershell
Compress-Archive -Path "C:\S21Lab\resultados_v22_gameserver\*" -DestinationPath "C:\S21Lab\Resultados_V22.zip" -Force
~~~

## Como interpretar

1. Janelas com muitos PCs inéditos sugerem exploração de novas regiões do runtime, sem comprovar progresso rumo ao OEP.
2. Janelas com quase zero PCs inéditos e alta repetição indicam ciclo mais concentrado, que deve ser investigado em termos de dispatch/VM.
3. Compare o PC final e o trace com a V21 para verificar que a nova instrumentação não desviou a trajetória.
4. Um alto valor em `runtime_coverage_top_pcs.csv` permite inspecionar a instrução e o contexto de seu bloco em vez de rastrear indiscriminadamente todas as seções.

**Nenhum dos resultados permite declarar recuperação das habilidades, fórmulas ou código do GameServer. Não executar PE Stage0 como programa.**
