# 0001 — Troca de contexto de fibers em assembly arm64 no iOS

Data: 2026-08-03
Status: aceito

## Contexto

O runtime agenda threads guest (Xbox 360) com fibers cooperativos:
`src/system/xthread.cpp` converte a thread host em fiber e faz `SwitchTo`
bidirecional. O backend POSIX (`src/core/fiber_posix.cpp`) usa
`makecontext`/`swapcontext` (ucontext), que **não existe no SDK do iOS** —
as funções não estão disponíveis para link.

## Decisão

Implementar a troca de contexto em assembly arm64 no estilo
boost.context/fcontext (~100 linhas de asm + glue), mantendo a API
`rex::thread::Fiber` idêntica. O mesmo backend pode servir macOS e iOS.

## Alternativas consideradas

- **ucontext no iOS** — indisponível no SDK; mesmo que linkasse, é API
  deprecada com custo extra de sigprocmask por switch.
- **Fibers emulados com threads** (thread parqueada em cond-var por fiber) —
  mais simples de escrever, mas adiciona latência de sincronização no caminho
  quente do scheduler e muda o timing do escalonamento cooperativo de forma
  sutil e difícil de depurar.

## Consequências

- Código assembly próprio para manter (arm64 apenas — suficiente para
  Mac Apple Silicon e iPad).
- Sem dependência nova e sem mudança de semântica: o resto do runtime não
  percebe a troca.
- Se o upstream mudar a API de Fiber, o backend precisa acompanhar.
