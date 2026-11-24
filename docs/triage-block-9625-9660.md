# Triagem do bloco #9625–#9660

Triagem realizada seguindo os critérios da issue #8919.

**Resumo:** open: 22 | close: 8 | merge: 10 | groups: 0 | kept: 5

## Recomendados para MERGE

- **#9627** — Preserva documento XML em `test_gamedata_xml.py` após declaração malformada. Diff limpo (+19), 14 tests OK.
- **#9629** — Proteção de escrita em `f16_trans_sweep.py` via arquivo temporário + `replace_retry`. 18 tests OK.
- **#9631** — Reconhece modelos Intel Core Ultra (245K, 255H, 285K) em `pc_checker.py`. `tests/test_pc_checker.py` OK.
- **#9635** — Rejeita índices de textura fora dos limites em `first_light.py` com mensagem clara + teste.
- **#9646** — Padroniza vocabulário de verificação na tabela de contrato em `docs/design.md` (rastreabilidade).
- **#9648** — Torna `--mesh-compute` e `--native-mesh` mutuamente exclusivos na CLI de `make_game.py`. 37 tests OK.
- **#9649** — Ignora tabelas Markdown dentro de fenced code blocks no `test_docs_table_rows.py`.
- **#9654** — Respeita tamanho de onda compilado no backend LLVM em `shader_disasm.py`. 78 tests OK.
- **#9658** — Segue saltos de linker stub (`jmp`) até o call site real em `import_call_sites.py`. 11 tests OK.
- **#9659** — Respeita ordem de limpeza das fixtures do CTest em `run_wine_tests.py`. 62 tests OK.

## Recomendados para CLOSE

- **#9625** — close: already closed / superseded (autor fechou antes da triagem)
- **#9628** — close: messy diff (mistura refatoração de mensagens com remoção de link do README)
- **#9638** — close: duplicate / superseded (já coberto por PRs mais recentes da suíte de build)
- **#9642** — close: dirty commit (título fala de code fence, mas remove 79 linhas de `pe_loader.cpp`)
- **#9643** — close: dirty commit (título fala de `bvh_slot_check.py`, mas reverte `pe_loader.cpp` e `xgameruntime.cpp`)
- **#9645** — close: decided against / out of scope (implementação nativa em Rust + Cargo, fora da convenção do projeto)
- **#9647** — close: dirty commit (mesma base corrompida de `pe_loader.cpp`)
- **#9650** — close: dirty commit (mesma base corrompida para mudança de 2 linhas em `bink_index.py`)

## Precisa de mais análise / humano (kept)

- **#9633** — `leonida_tui`: decode UTF-8 com fallback. Conflita com grupo TUI. Validar unificação.
- **#9637** — docs: remove seções duplicadas + teste de `repeated_blocks`. Precisa alinhamento com equipe de docs.
- **#9653** — runtime: lista llvm-mc shader tests como Not Run. Validar em Linux/CI.
- **#9655** — `xgameruntime`: parse de title ID e locale. Precisa teste Windows + MSVC.
- **#9660** — `d3d12_xs`: QueryResourceResidency. Toca em D3D12 → coordenar com #313.

agent: yes
