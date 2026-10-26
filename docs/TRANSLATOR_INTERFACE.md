# The translator interface

`ps4_app` is built from your own copy of the game's executable, statically translated from x86-64 to
ARM64. **The translator itself is not part of this repository.** The code refers to it as *PS4Recomp*:
comments in `runtime/core/recomp_context.h` point at `pkg/lifter` and `pkg/emitter/asm_trampoline.go`, which
are Go packages of that project (an ELF loader, disassembler, analyzer, lifter and ARM64 emitter). This
document records what the build expects the translator to produce, so the pipeline can be reproduced or
reimplemented. Everything below was read from the files this repository consumes; nothing game-specific
is reproduced here.

```
game executable (ELF) --> translator --> guest_image.bin
                                     --> code_NNN.s, code_NNN_reg.c   (NNN = 000, 001, ...)
                                     --> dispatch.c
                                     --> guest_modules.c, guest_modules_synth.s
                       tools/peephole_guest_code.py: code_NNN.s --> code_NNN.opt.s
                       tools/generate_build_ninja.py + ninja: --> ps4_app
```

`tools/generate_build_ninja.py` discovers the inputs by name in the repository root and never creates
them. `.gitignore` keeps all of them out of Git, because they are derived from the game.

## Inputs the build consumes

| File | What it is |
|---|---|
| `guest_image.bin` | Flat guest memory image of the executable. `main.c` loads it with `recomp_init_runtime_file("guest_image.bin", ...)`. `build_and_run.sh` refuses to start without it. |
| `code_NNN.s` | ARM64 assembly for one *chunk* of translated functions. The first line is a comment such as `// Chunk 0 (N functions)`, followed by `.section __TEXT,__recomp_guest,regular,pure_instructions`. The build globs `code_[0-9]+.s`; `code_NNN.opt.s` is the peephole output. |
| `code_NNN_reg.c` | For the same chunk: `extern void fn_0x<VA>(GuestContext *ctx);` declarations and an init function that calls `recomp_register_fn(0x<VA>ULL, fn_0x<VA>)` for each function. |
| `dispatch.c` | `recomp_init_dispatch_chunk_N()` declarations and the function that calls them all, so every chunk registers its functions at start-up. |
| `guest_modules.c` | `recomp_register_guest_modules()`: one `recomp_register_unresolved(VA, "<nid>", "<name>", "<library>")` per imported system symbol, plus forward declarations and weak fallbacks of the `shim_*` host functions. |
| `guest_modules_synth.s` | ARM64 bridge functions for synthetic shims, in the same `__recomp_guest` section. |

## Conventions the runtime and the peephole pass rely on

- **Function symbols.** Each translated guest function is `_fn_0x<guest VA in hex>` (C name
  `fn_0x<VA>`), takes a `GuestContext *` and is registered under its guest virtual address.
- **Labels and comments.** Function labels look like `_fn_0x<VA>:`, and every guest instruction is
  annotated with a comment `// 0x<VA>: <mnemonic operands>`. `tools/peephole_guest_code.py` matches exactly
  these (`FUNCTION` and `GUEST_ADDRESS` in that script). It also matches guest memory accesses of the form
  `ldr`/`str ... [x28, x16]` or `[x28, x17]`, where `x16` and `x17` hold the guest address, to add or drop
  the TSO boundary check. Local labels inside a function are named `L_fn_0x<VA>_loc_0x<VA>` in the code this
  repository was built from. Changing any of these breaks the peephole rewrites.
- **Registers.** The generated code keeps the guest stack pointer and frame pointer in ARM64 registers and
  addresses guest memory as `[x28, x<reg>]`, with `x28` the guest memory base. In the code this repository
  was built from, `x26` holds RSP and `x25` holds RBP, and `x19` and `x21` hold RAX and RCX. Treat the
  mapping as defined by the translator that produced your `code_NNN.s`.
- **GuestContext layout.** Offsets of `GuestContext` fields are hardcoded in the translator's assembly
  (the trampoline and the lifter's inline sequences for the x87 stack, MXCSR, flag bytes and the TCB
  slot). `runtime/core/recomp_context.h` pins them with `RECOMP_CTX_OFFSET(...)` static assertions, so a
  mismatch fails at build time. `RECOMP_TCB_CTX_SLOT` (`0x70`) is the TCB slot that points at the thread's
  `GuestContext`.
- **Trampoline.** `trampoline_arm64.s` (tracked) is the host-to-guest entry; the contract is described at
  `recomp_enter_guest` in `recomp_context.h`.
- **Memory ordering.** x86 loads and stores that may cross a 16-byte boundary are handled by
  `runtime/core/tso_crossing.c` and the `tools/tso_crossing_sites.txt` list; see the README.

## Reproducing or replacing it

A working translator must emit the files above with those conventions, against the runtime in `runtime/`.
`tools/generate_build_ninja.py` shows how the generated files are discovered, and
`tests/test_build_system_idempotence.py` checks that the generated build graph is deterministic.

If you have the original translator, put its output in the repository root (or a directory you pass to
the build tooling) and follow the README's build steps. If you maintain it as a separate repository, link
it from the README so others can find it.
