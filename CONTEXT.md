# CONTEXT — vocabulário do port iPadOS

Glossário dos termos como usados neste projeto (port pessoal do
GoldenEye Metal para iPad). Docs do port em `docs/ipad/`.

## Termos

**Backup (do jogo)** — o arquivo bruto do GoldenEye XBLA que o dono fornece:
ZIP original, pacote STFS/Xbox LIVE, ou pasta extraída com `default.xex`,
`files/`, `music.xwb` e `sfx.xwb`. Nunca versionado, nunca incluído no app.

**Game data (importado)** — o resultado da importação/validação do backup pelo
launcher macOS. É o diretório que o runtime consome via `game_data_root`.
Distinto do backup: o backup é insumo bruto; o game data é o formato pronto
para o jogo.

**Baseline** — o build macOS funcionando no Mac do dono com o próprio backup.
Referência de comparação: qualquer defeito que apareça no iPad e não no
baseline é defeito do port.

**Port** — o conjunto de mudanças específicas de iPadOS sobre o código do
upstream: backend de janela UIKit, fibers em asm, bootstrap sem launcher,
empacotamento ipa.

**Upstream** — o repositório ysrdevs/goldeneye-metal. Este projeto é pessoal e
não tem compromisso de contribuir de volta.

**Guest / host** — código do jogo Xbox 360 recompilado (guest) vs o runtime
nativo que o hospeda (host). Terminologia herdada do upstream/Xenia.

**Bootstrap (iPad)** — o ponto de entrada mínimo do app no iPad que substitui o
launcher: valida o game data em Documents e inicia o jogo direto.
