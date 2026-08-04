# GoldenEye Metal — port iPadOS

Documento de trabalho do port pessoal para iPad. Atualizado durante a sessão de
planejamento (2026-08-03).

## Decisões resolvidas

| Decisão | Valor | Notas |
| --- | --- | --- |
| Localização do projeto | `/Volumes/NVME 500GB/goldeneye-metal` | Clone completo do ysrdevs/goldeneye-metal. Atenção: espaço no path já quebrou tooling no passado (GeneralsX) — resolver com symlink se algum script engasgar. |
| Objetivo | Pessoal, sem cerimônia | Sem compromisso com upstream. Hacks aceitáveis quando acelerarem. Docs só para referência própria. |
| iPad alvo | Chip A-series (A14–A17) | GPU family Apple7/8/9 — A14 é a mesma família do M1, feature set Metal próximo do validado no macOS. Restrição principal: 4 GB de RAM (limite por app ~2 GB sem entitlement de memória estendida). |
| Game backup | OK — pacote STFS verificado | `/Volumes/NVME 500GB/GoldenEye_007_XBLA_Retail/0000000000000000/584108A9/000D0000/30BA...617` (739 MB, magic LIVE, title ID 584108A9). Nota: a primeira tentativa (`Bond2011XE`) era o GoldenEye Reloaded 2011 da Eurocom — jogo errado, incompatível por construção. |
| Camada de janela iOS | UIKit custom | `window_ios.mm` + `windowed_app_main_ios.mm` seguindo o padrão de `window_macos.mm`: CAMetalLayer sobre UIView, fullscreen fixo. SDL3 segue só para input/audio (não há caminho de vídeo SDL no repo). |
| Fibers no iOS | Asm arm64 fcontext | Ver `docs/ipad/adr/0001-fibers-asm-arm64.md`. |
| Sequência | Baseline macOS primeiro | Compilar e rodar a versão macOS com o backup próprio antes do port. Prova a revisão do backup, a toolchain e isola bugs futuros do iPad como bugs do port. |
| Launcher no iPad | Não portar | Import feito no Mac (launcher macOS do baseline); a pasta de game data importada vai por AirDrop para Documents do app (UIFileSharingEnabled, esquema GeneralsX). O app iPad boota direto no jogo com `game_data_root` → Documents, com bootstrap mínimo de validação (`ValidateImportedDirectory`). |
| Input | Controle físico Bluetooth apenas | Driver SDL3 de gamepad já existe e suporta GCController no iOS. Teclado/mouse (AppKit) e touch overlay ficam fora do escopo inicial. |
| Assinatura/distribuição | SideStore, pipeline GeneralsX | Team pessoal `SNS5MZ4U4A`, bundle ID sugerido `digital.coopers.goldeneye`. Re-sign semanal on-device. |

## Decisões deferidas (resolver durante execução)

- **Entitlement de memória estendida** — decidir medindo o pico real no device;
  guest de 512 MB + runtime deve caber nos ~2 GB padrão.
- **Modelo exato do iPad / versão mínima do iPadOS** — confirmar em Ajustes →
  Geral → Sobre; assumindo iPadOS 17+ até lá.
- **FFmpeg para iOS** — thirdparty precisa compilar para o target iOS
  (usado no caminho de áudio XMA); avaliar esforço no primeiro build.

## Roadmap

1. **Baseline macOS** — trazer o backup para o Mac, buildar e rodar a Dam no
   Mac. Valida backup + toolchain.
   - ✅ 2026-08-03: app oficial v0.4.1 instalado, pacote STFS importado pelo
     launcher e Dam rodando no Mac (baseline de runtime validado).
   - `default.xex` importado copiado para `vendor/GoldenEye-Recomp/assets/`.
   - Game data importado: `~/Library/Application Support/GoldenEye Metal/Game Data`.
   - ✅ Build de fonte completo: SDK/rexglue → `codegen` (2,4s, 35 arquivos
     gerados) → target `ge` → binário dev rodando com o game data importado
     (render Metal ativo, shutdown limpo). **Fase 1 concluída.**
   - Gotcha confirmado: configure falha sem `git submodule update --init --recursive`
     (mesma pegadinha do GeneralsX).
   - Gotcha novo: `cmake --build --parallel` sem limite congela o Mac (10 cores,
     16 GB) — usar `--parallel 4` + `nice -n 15`.
   - Comando de execução dev (referência):
     `DYLD_LIBRARY_PATH="$PWD/out/macos-arm64" REX_INPUT_BACKEND=sdl REX_MNK_MODE=true ./vendor/GoldenEye-Recomp/out/build/macos-arm64-release/GoldenEye --game_data_root "$HOME/Library/Application Support/GoldenEye Metal/Game Data" --gpu metal`
2. **Toolchain iOS** — preset CMake para iOS device arm64; fazer thirdparty
   (FFmpeg, SDL3, etc.) compilar; stub da camada de janela.
   - ✅ 2026-08-03: SDK completo compilado e linkado para iOS
     (`out/ios-arm64/`). Correções necessárias: gate do CLI rexglue (host
     tool), `CMAKE_SYSTEM_PROCESSOR=arm64` no preset (senão FFmpeg NEON fica
     de fora e o link falha — e o valor congela no primeiro configure, exige
     build dir limpo), shim UIKit p/ SDL3 sem vídeo (`SDL_IsIPad`),
     `displaySyncEnabled` é macOS-only, `REX_PLATFORM=ios-arm64` (senão
     artefatos iOS sobrescrevem `out/macos-arm64` — aconteceu; restaurar com
     rebuild macOS).
3. **Portes de código** — fibers asm arm64 (ADR 0001); `window_ios.mm` +
   `windowed_app_main_ios.mm` (CAMetalLayer/UIKit); bootstrap sem launcher.
   - ✅ Fibers asm: testes [fiber] passam; jogo macOS validado 3 min no mesmo
     backend. Falhas pré-existentes de testes de memória neste macOS 26
     (baseline sem mudanças falha igual).
   - ✅ UIKit: window_ios.mm (CADisplayLink pump) + entry point
     UIApplicationMain com game_data_root → Documents. Compilam e linkam;
     validação real só no device.
4. **Empacotamento** — ipa dev assetless via pipeline GeneralsX (`xattr -cr` +
   `zip -X`), instalar via SideStore.
   - ✅ 2026-08-03: `GoldenEye-iPad.ipa` (11 MB) gerado por
     `scripts/build/ios/package-ipa.sh` — dylib do runtime embutido em
     Frameworks/, Info.plist com UIFileSharingEnabled + UILaunchScreen +
     landscape-only, assinatura ad-hoc (SideStore re-assina na instalação).
   - Rebuild completo iOS (referência):
     `cmake --preset ios-arm64-release && cmake --build out/build/ios-arm64-release --parallel 4`,
     depois `cmake -S vendor/GoldenEye-Recomp --preset ios-arm64-release &&
     cmake --build vendor/GoldenEye-Recomp/out/build/ios-arm64-release --target ge --parallel 4`,
     e `./scripts/build/ios/package-ipa.sh`.
   - Atenção: `vendor/GoldenEye-Recomp/generated/rexglue.cmake` é regenerado
     pelo codegen e carrega um guard local (`if(TARGET rex::rexglue)`); o
     template do SDK já foi corrigido, mas se o codegen upstream rodar sem o
     template novo, reaplicar o guard.
5. **Dados + primeiro boot** — importar no Mac, AirDrop para Documents, bootar
   a Dam no iPad. Medir memória e FPS; decidir entitlement.
   - Pendente (única fase que exige o iPad físico): instalar o ipa via
     SideStore, AirDropar a pasta `Game Data` (de
     `~/Library/Application Support/GoldenEye Metal/`) para "On My iPad →
     GoldenEye" via Files, e abrir o app.

## Riscos

- Backup pode não ser da revisão suportada — descoberto cedo pelo baseline.
- GPU A-series (Apple7/8) vs M-series: resolves de MSAA/depth validados só em
  M-series; possíveis fallbacks necessários.
- 4 GB de RAM no device: pico de memória pode exigir entitlement ou cortes.
- Espaço no path `/Volumes/NVME 500GB` pode quebrar scripts de build do repo —
  mitigação: symlink sem espaço.

## Restrições herdadas da análise do código

- Recompilação é 100% AOT — sem JIT, compatível com iOS por construção.
- O C++ recompilado do jogo NÃO está no repo: é gerado no build a partir do
  backup do jogo (XEX/STFS) fornecido pelo usuário.
- Camada de janela é AppKit (`src/ui/window_macos.mm`, `surface_macos.mm`,
  `windowed_app_main_macos.mm`) — precisa de backend iOS/UIKit.
- Fibers usam `ucontext` (`src/core/fiber_posix.cpp`) — API inexistente no SDK
  do iOS; precisa de substituto (asm arm64 ou threads).
- SDL3 já é usado para input e áudio (suporta iOS).
- Compilação de MSL em runtime (`msl_compiler.mm`) é permitida no iOS.
- Distribuição: sideload via SideStore (pipeline do GeneralsX reaproveitável —
  team `SNS5MZ4U4A`, gotchas de `xattr -cr` + `zip -X`).

## Decisões em aberto

- Disponibilidade do backup do jogo (bloqueador de build).
- Abordagem da camada de janela: UIKit custom vs backend de vídeo do SDL3.
- Substituto dos fibers: asm context-switch vs threads.
- Validar build macOS primeiro como baseline?
- Bundle ID, entitlement de memória, modelo exato do iPad.
