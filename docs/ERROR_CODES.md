# ViperLang Error Codes

This file is the canonical error-code catalog for ViperLang.

Goal:

- keep runtime logs short
- keep codes stable
- let humans and LLMs jump from `code -> cause -> fix`

## Format

ViperLang errors use:

```text
Kind [CODE]: short message
Stack trace:
  at ...
```

Example:

```text
Runtime Error [VRT019]: Array index out of bounds.
```

Kinds:

- `VM Panic`
- `Runtime Error`
- `TypeError`
- `Panic`

Prefixes:

- `VVM` = VM / interpreter panic
- `VRT` = runtime contract violation
- `VTP` = type assertion failure
- `VNT` = native runtime / stdlib misuse
- `VFF` = FFI signature / dynamic call setup
- `VAI` = AI native surface
- `VCL` = top-level CLI input/output failure
- `VBL` = app build / packaging pipeline failure
- `VIX` = semantic indexer / state / diff / query failure
- `VPK` = package manager / ABI workflow failure
- `VCP` = compiler / static semantic failure
- `VAS` = async I/O runtime infrastructure failure
- `VME` = memory subsystem fatal failure

## LLM Usage Rule

When a log contains a bracketed code:

1. trust the code before the prose
2. look up the code in `docs/llm/ERRORS.md`
3. use this detailed file only if the short LLM file is not enough

## VM Panic Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VVM001` | VM out of memory | register/frame/object growth failed | reduce runaway allocation, inspect recursion, inspect huge arrays/strings |
| `VVM002` | instruction pointer out of bounds | bad jump, corrupted frame, invalid bytecode | inspect recent control-flow lowering, callback/async frame handling, bytecode emission |
| `VVM003` | division by zero | numeric `/` with zero divisor | guard divisor or pre-check in caller |
| `VVM004` | unknown opcode | corrupted bytecode or emitter bug | inspect compiler opcode emission and bytecode loading |

## Runtime Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VRT001` | FFI struct arg expected instance | non-struct value passed to struct FFI arg | pass a matching Viper struct instance |
| `VRT002` | FFI struct field count mismatch | C struct signature expects more fields than object has | align Viper struct shape with C signature |
| `VRT003` | FFI int32 arg mismatch | string/object passed to `i` arg | pass number/bool or fix signature |
| `VRT004` | FFI int64 arg mismatch | wrong value for `I` arg | pass number/bool or fix signature |
| `VRT005` | FFI float arg mismatch | wrong value for `f` arg | pass number/bool or fix signature |
| `VRT006` | FFI double arg mismatch | wrong value for `d` arg | pass number/bool or fix signature |
| `VRT007` | FFI pointer conversion unsupported | unsupported object/value passed to `p` arg | pass string, dl handle, pointer, or nil |
| `VRT008` | FFI arg type unsupported | unsupported internal ffi arg type | inspect `get_fn` signature and native ffi type lowering |
| `VRT009` | call arity mismatch | wrong argument count at function call | align call-site with function arity |
| `VRT010` | struct constructor arity mismatch | too many fields passed while calling a struct | pass at most the declared field count |
| `VRT011` | dynamic FFI arity mismatch | wrong arg count for `get_fn(...)` result | align call-site with FFI signature |
| `VRT012` | object not callable | tried to call a non-callable object | inspect callee expression and late binding |
| `VRT013` | plain value not callable | tried to call number/bool/nil | inspect callee expression |
| `VRT014` | field target must be instance | field get/set on non-instance | guard target type or fix receiver |
| `VRT015` | undefined field | unknown struct field name | fix field spelling or struct definition |
| `VRT016` | native disabled by capability | code called a native removed by build profile | enable capability or avoid native |
| `VRT017` | unknown native index | bytecode/native registry mismatch | inspect native registration stability |
| `VRT018` | array index type mismatch | non-number index on array | use numeric index |
| `VRT019` | array index out of bounds | index < 0 or index >= len | guard index or inspect loop bounds |
| `VRT020` | instance index key must be string | non-string `obj[key]` on instance | use string field key |
| `VRT021` | unsupported index target | index access on unsupported type | only arrays/instances are indexable |
| `VRT022` | spawn target invalid | `spawn` on non-function | pass function call to `spawn` |
| `VRT023` | spawn arity mismatch | wrong arg count in `spawn` | align spawned call arity |
| `VRT024` | spawn allocation failed | child fiber setup failed | inspect memory pressure |
| `VRT025` | await target invalid | `await` on non-thread | only await thread handles |
| `VRT026` | try stack overflow | too many nested try blocks | flatten nesting or raise handler limit |
| `VRT027` | eval expects string | non-string passed to `eval()` | pass source string |
| `VRT028` | eval parse failed | generated source is invalid | inspect eval input text |
| `VRT029` | eval compile failed | parsed AST failed during compile | inspect eval code for unsupported constructs |

## Type Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VTP001` | runtime type assertion failed | annotation/assertion does not match runtime value | fix source type, annotation, or conversion |

## Native Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VNT001` | `load_dl` arg invalid | non-string path passed to loader | pass library path string |
| `VNT002` | `load_dl` failed | bad path or missing shared object | verify library path and loader visibility |
| `VNT003` | `serve` args invalid | wrong `(port, response)` shape | pass numeric port and string/function |
| `VNT004` | `serve` socket failed | socket creation failed | inspect OS/socket permissions |
| `VNT005` | `serve` bind failed | port in use or permission denied | free port or use allowed port |
| `VNT006` | `serve` listen failed | socket listen setup failed | inspect socket state |
| `VNT007` | `fetch` arg invalid | non-string URL | pass URL string |
| `VNT008` | `write` args invalid | wrong `(path, content)` shape | pass two strings |
| `VNT009` | `query` arg invalid | non-string SQL | pass SQL string |
| `VNT010` | `env` arg invalid | non-string env key | pass env key string |
| `VNT011` | `os.cron` schedule invalid | unsupported cron format | use `*`, `*/N`, or `@every 10s|5m|1h` |
| `VNT012` | `web.serve` args invalid | wrong `(port, handler?)` shape | pass port and optional handler |
| `VNT013` | `web.serve` socket failed | socket creation failed | inspect OS/socket permissions |
| `VNT014` | `web.serve` bind failed | port in use or permission denied | free port or use allowed port |
| `VNT015` | `web.serve` listen failed | listen setup failed | inspect socket state |
| `VNT100` | OS capability disabled | code called disabled OS native | enable OS capability/profile |
| `VNT101` | FS capability disabled | code called disabled FS native | enable FS capability/profile |
| `VNT102` | Web capability disabled | code called disabled Web native | enable Web capability/profile |
| `VNT103` | DB capability disabled | code called disabled DB native | enable DB capability/profile |
| `VNT104` | AI capability disabled | code called disabled AI native | enable AI capability/profile |
| `VNT105` | Cache capability disabled | code called disabled Cache native | enable Cache capability/profile |
| `VNT106` | Util capability disabled | code called disabled Util native | enable Util capability/profile |
| `VNT107` | Meta capability disabled | code called disabled Meta native | enable Meta capability/profile |

## FFI Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VFF001` | `get_fn` args invalid | wrong `(dl_handle, fn_name, signature)` shape | pass `(handle, string, string)` |
| `VFF002` | symbol not found | `dlsym` failed | verify exported function name |
| `VFF003` | signature separator invalid | missing `->` | use `arg_types->ret_type` |
| `VFF004` | return type invalid | bad return type or illegal `v` arg use | fix signature type set |
| `VFF005` | trailing return type garbage | extra chars after parsed return type | remove suffix garbage |
| `VFF006` | signature build OOM | allocation failed while parsing signature | inspect memory pressure |
| `VFF007` | unexpected comma | signature starts list item with `,` | remove extra comma |
| `VFF008` | missing comma | adjacent types with no separator | insert comma |
| `VFF009` | argument type invalid | unknown arg type token | use supported ffi tokens |
| `VFF010` | trailing comma | arg list ends with `,` | remove trailing comma |
| `VFF011` | cif allocation failed | failed to allocate call interface | inspect memory pressure |
| `VFF012` | `ffi_prep_cif` failed | invalid libffi call interface | inspect signature compatibility |
| `VFF013` | signature copy OOM | failed storing signature metadata | inspect memory pressure |

## AI Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VAI001` | unsupported AI provider for `ai.ask` | provider is not `openai` | configure supported provider or avoid `ai.ask` |

## CLI Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VCL001` | source file open failed | bad path or missing file | verify entry path |
| `VCL002` | source file allocation failed | huge file or OOM | inspect file size / memory pressure |
| `VCL003` | source file read failed | partial read or fs error | inspect permissions and file integrity |
| `VCL004` | bytecode file load failed | missing or corrupt `.vbc/.vbb` | rebuild artifact or verify path |
| `VCL005` | bytecode emit failed | output path invalid or fs write failed | verify output path and permissions |

## Build Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VBL001` | build entry file not found | bad source entry path | point build command at an existing `.vp` file |
| `VBL002` | Makefile missing in current directory | build ran outside repo root | run build from Viper source root |
| `VBL003` | build hint only | helper hint after `VBL002` | move to source root before retry |
| `VBL004` | output path too long | app name / output dir produced oversized path | shorten app name or output path |
| `VBL005` | output dir create failed | permission error or invalid path | inspect destination directory |
| `VBL006` | bytecode write failed | could not write `app.vbb` | inspect destination permissions/disk |
| `VBL007` | capability lock write failed | could not write `capabilities.lock` | inspect destination permissions/disk |
| `VBL008` | runtime path not shell-safe | generated runtime path contains unsafe chars | choose safer output dir/app name |
| `VBL009` | runtime build failed | `make runtime` failed | inspect build toolchain and profile flags |
| `VBL010` | launcher write failed | could not write `run.sh` | inspect destination permissions/disk |

## Indexer Codes

### Index Core

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VIX001` | file open failed | bad source/state path | verify file path |
| `VIX002` | file seek failed | non-seekable or corrupt file handle | inspect filesystem/path |
| `VIX003` | file size lookup failed | fs metadata error | inspect file and permissions |
| `VIX004` | read allocation failed | OOM while buffering file | inspect memory pressure |
| `VIX005` | file read failed | partial read / fs error | inspect file integrity |
| `VIX006` | module slot allocation failed | project/module list growth failed | inspect memory pressure |
| `VIX007` | module parse failed | source contains syntax error | inspect parser error at target file |
| `VIX008` | import resolution failed | unresolved relative/std import | verify import path and project root |
| `VIX009` | focus slice allocation failed | OOM while building focus graph | inspect memory pressure / project size |
| `VIX010` | focus symbol not found | requested symbol absent from slice | query the symbol name first |
| `VIX011` | impact slice allocation failed | OOM while building reverse call graph | inspect memory pressure / project size |
| `VIX021` | entry path resolution failed | CLI entry path invalid | pass a real entry file |
| `VIX022` | project root resolution failed | repo marker/root not found | ensure run is inside a Viper project |
| `VIX023` | output file open failed | bad output path | inspect output file permissions |

### State Manifest And Resume

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VIX012` | state file open failed | missing `.vstate` | verify state path |
| `VIX013` | state file empty | truncated/empty state artifact | regenerate state |
| `VIX014` | unsupported state format | wrong or old state version | regenerate with current Viper |
| `VIX015` | malformed tracked file entry | corrupt `tracked_files` row | regenerate state |
| `VIX016` | state manifest allocation failed | OOM while reading manifest | inspect memory pressure |
| `VIX017` | malformed tracked test entry | corrupt `tracked_tests` row | regenerate state |
| `VIX018` | incomplete state file | missing required root/entry fields | regenerate state |
| `VIX019` | corrupt tracked file list | file/hash counts differ | regenerate state |
| `VIX020` | corrupt tracked test list | test/hash counts differ | regenerate state |
| `VIX024` | resume input load failed | manifest/hash verification failed | verify state integrity |
| `VIX025` | stale module map failed | stale files could not map to modules | inspect state/module ledger |
| `VIX026` | symbol ledger load failed | embedded symbol ledger missing/corrupt | regenerate state |
| `VIX027` | focused resume selection failed | focus symbol could not be selected | verify focus symbol exists |
| `VIX028` | verification summary emit failed | output writer/state summary error | inspect output path/state integrity |
| `VIX029` | stale symbol emit failed | stale symbol section could not be built | inspect ledger/module refs |
| `VIX030` | embedded symbol ledger missing | state lacks symbol ledger section | regenerate state |
| `VIX031` | brief resume emit failed | compact resume output failed | inspect output path/state integrity |
| `VIX032` | linked test emit failed | linked tests could not be emitted | inspect `@covers` ledger/state |

### Query / Benchmark / Verify / Refresh

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VIX033` | state query missing filter | no `name/effect/call` filter provided | add at least one query filter |
| `VIX034` | state query manifest load failed | query could not validate/load state | inspect state path/integrity |
| `VIX035` | query stale module map failed | stale files could not map to modules | inspect state/module ledger |
| `VIX036` | query symbol ledger failed | ledger load or slice selection failed | inspect filters and state ledger |
| `VIX037` | stale module emit failed | stale module section emit failed | inspect output path/state integrity |
| `VIX038` | query path explain emit failed | BFS/path proof generation failed | inspect selected symbol graph |
| `VIX039` | risk ranking emit failed | risk computation/output failed | inspect symbol graph / output path |
| `VIX040` | verbose query emit failed | full query ledger output failed | inspect output path/state integrity |
| `VIX041` | brief query emit failed | compact query output failed | inspect output path/state integrity |
| `VIX042` | benchmark dependency query missing | `--query-deps` used without query filter | add a query filter |
| `VIX043` | benchmark input load failed | state or ledger load failed | inspect state path/integrity |
| `VIX044` | benchmark slice selection failed | focus/query selection failed | verify focus symbol or query filter |
| `VIX045` | benchmark metric compute failed | ledger metric pass failed | inspect selected slice/state integrity |
| `VIX046` | state verify failed | manifest/hash verify failed | inspect state path/integrity |
| `VIX047` | verify/refresh stale map failed | changed files could not map to modules | inspect state/module ledger |
| `VIX048` | state refresh failed | refresh pipeline failed before rewrite | inspect tracked modules and entry |
| `VIX049` | state proof load failed | `.vproof` missing/corrupt | regenerate proof or rerun targeted tests |
| `VIX050` | temp state path alloc failed | could not allocate temp path | shorten path / inspect memory |
| `VIX051` | refreshed state reload failed | new state written but unreadable | inspect refreshed file integrity |
| `VIX052` | refreshed module map failed | refreshed stale-module mapping failed | inspect state/module ledger |
| `VIX053` | proof path alloc failed | could not build proof path | shorten path / inspect memory |
| `VIX054` | proof refresh failed | linked test rerun or proof write failed | inspect linked tests and proof path |
| `VIX055` | refresh patch emit failed | delta patch output failed | inspect output path/state integrity |
| `VIX056` | state replace failed | final state rename/replace failed | inspect filesystem permissions |
| `VIX057` | proof replace failed | final proof rename/replace failed | inspect filesystem permissions |

### State Plan / Test Run / Semantic Diff

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VIX058` | state plan missing before/after | one or both state inputs missing | pass both state files |
| `VIX059` | state plan inputs load failed | manifest/semantic rows could not load | inspect both states |
| `VIX060` | state plan config mismatch | focus/impact settings differ between states | regenerate with matching settings |
| `VIX061` | state test run missing before/after | one or both state inputs missing | pass both state files |
| `VIX062` | state test inputs build failed | could not build test plan candidates | inspect state artifacts and linked tests |
| `VIX063` | state test config mismatch | focus/impact settings differ | regenerate with matching settings |
| `VIX064` | state test runner prep failed | root/path resolution failed | inspect state roots and paths |
| `VIX065` | semantic diff missing inputs | before/after/focus not fully provided | pass both entries and focus symbol |
| `VIX066` | diff focus symbol missing | focus symbol absent from both inputs | query available symbols first |
| `VIX067` | semantic diff item build failed | semantic collection failed | inspect parse/index state of inputs |
| `VIX068` | semantic diff row build failed | stable row conversion failed | inspect semantic item integrity |
| `VIX069` | semantic change plan emit failed | change-plan output failed | inspect diff rows / output path |
| `VIX070` | semantic test plan emit failed | linked-test plan output failed | inspect diff rows / linked tests |

## Package Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VPK001` | invalid ABI line | malformed `.vabi` row | regenerate ABI file |
| `VPK002` | invalid ABI kind | unknown `fn/st` tag | regenerate ABI file |
| `VPK003` | invalid ABI metric | malformed arity/field count | regenerate ABI file |
| `VPK004` | conflicting ABI symbol | duplicate ABI symbol with mismatched contract | rebuild package artifacts |
| `VPK005` | ABI file parse failed | malformed `.vabi` file | rebuild package artifacts |
| `VPK006` | bytecode read failed for ABI verify | missing/corrupt `.vbb` | rebuild package artifacts |
| `VPK007` | bytecode symbol collection failed | corrupt bytecode or symbol extraction failure | rebuild package artifacts |
| `VPK008` | ABI missing symbol | expected exported symbol absent from bytecode | rebuild package or inspect exports |
| `VPK009` | ABI metric mismatch | arity/field count drifted from ABI | rebuild package or inspect breaking change |
| `VPK010` | ABI file missing for module | `.vbb` exists without `.vabi` | rebuild package artifacts |
| `VPK011` | conflicting ABI entry in tree walk | duplicate package ABI entry with different contract | inspect package artifact tree |
| `VPK012` | package shell path too long | quoted clone path too long | shorten project/package path |
| `VPK013` | clone command too long | generated git clone command overflowed buffer | shorten package path/url/version |
| `VPK014` | package branch string too long | branch/version too long for clone command | shorten version/branch value |
| `VPK015` | remote fetch failed | git clone failed | inspect repo URL/branch/network |
| `VPK016` | docs pack path resolve failed | onboarding pack target path invalid | inspect project path |
| `VPK017` | docs pack dir create failed | could not create `docs/llm` | inspect permissions |
| `VPK018` | cwd resolve failed | `getcwd` failed | inspect filesystem state |
| `VPK019` | path too long | manifest/lock/package path overflowed | shorten project/package path |
| `VPK020` | manifest write failed | could not write `viper.vpmod` | inspect permissions/disk |
| `VPK021` | package store create failed | could not create `.viper/packages` | inspect permissions/disk |
| `VPK022` | lock init failed | could not initialize `viper.lock` | inspect permissions/disk |
| `VPK023` | invalid package name | bad `name` syntax in CLI input | use a valid package identifier |
| `VPK024` | invalid version | bad version string | use `latest` or valid version |
| `VPK025` | project root missing | no `viper.vpmod` in cwd chain | run `viper pkg init` first |
| `VPK026` | manifest update failed | dependency add/remove write failed | inspect manifest permissions/format |
| `VPK027` | install-after-add failed | add succeeded but install pipeline failed | inspect package fetch/build logs |
| `VPK028` | remove refresh failed | remove succeeded but install/lock refresh failed | inspect package store/lock generation |
| `VPK029` | install failed | dependency install pipeline failed | inspect package fetch/build logs |
| `VPK030` | manifest dependency read failed | manifest parse/read failed | inspect `viper.vpmod` format |
| `VPK031` | lock write failed | could not write `viper.lock` | inspect permissions/disk |
| `VPK032` | manifest read failed | manifest could not be read/parsed | inspect `viper.vpmod` |
| `VPK033` | package build failed | package artifacts could not be built | run `viper pkg install` and inspect package sources |
| `VPK034` | installed package path missing | manifest/lock references absent package dir | rerun install or inspect store |
| `VPK035` | ABI check filesystem failure | tree walk/path operation failed | inspect package store integrity |
| `VPK036` | ABI mismatch found | one or more module ABI checks failed | inspect listed module mismatches |
| `VPK037` | invalid ABI diff range | bad `from -> to` version pair | pass valid versions |
| `VPK038` | package version missing | requested package version dir absent | install or fetch that version |
| `VPK039` | ABI diff input load failed | ABI entries for compare could not load | rebuild/install both versions |
| `VPK040` | breaking ABI changes detected | diff found breaking changes under fail mode | inspect diff output and bump version |

## Compiler Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VCP001` | native disabled in current profile | code called a native removed by capability profile | enable capability or avoid that native |
| `VCP002` | compiler out of memory | huge module graph or allocator failure | inspect memory pressure / project size |
| `VCP003` | package import contains `..` | unsafe package path | remove parent traversal from import |
| `VCP004` | stdlib module not found | missing `@std/...` module or wrong std path | inspect `VIPER_STD_PATH` and installed stdlib |
| `VCP005` | invalid package import syntax | malformed package import path | use valid `@pkg[/module]` form |
| `VCP006` | package path too long | resolved package path overflowed buffer | shorten project/package path |
| `VCP007` | package module not found | package source missing from store | run `viper pkg install` or fix import |
| `VCP008` | module path too long | raw `use` path overflowed buffer | shorten import path |
| `VCP009` | resolved module path too long | importer base + relative path too long | shorten path layout |
| `VCP010` | namespace alias too long on import | alias exceeds namespace buffer | shorten alias |
| `VCP011` | namespaced symbol conflict | same alias+name maps to different object | rename alias or exported symbol |
| `VCP012` | unknown type annotation | annotation is not builtin or known struct | fix annotation or import the type |
| `VCP013` | compile-time type mismatch | inferred type conflicts with declared type | align annotation/initializer/return |
| `VCP014` | declared effects incomplete | inferred effect set exceeds `@effect(...)` | add missing effect(s) |
| `VCP015` | invalid ABI metric | bad arity/field count in ABI | regenerate/fix ABI entry |
| `VCP016` | conflicting ABI entries | duplicate ABI symbol with different contract | fix exported ABI contract |
| `VCP017` | invalid ABI entry | malformed ABI row | regenerate/fix ABI file |
| `VCP018` | invalid ABI symbol kind | non-`fn/st` symbol kind in ABI | regenerate/fix ABI file |
| `VCP019` | invalid ABI symbol name | malformed empty/oversized symbol name | regenerate/fix ABI file |
| `VCP020` | duplicate ABI return type field | repeated `ret=` in ABI row | keep a single return type field |
| `VCP021` | invalid ABI effects field | malformed `eff=` list | use valid comma-separated effect names |
| `VCP022` | bytecode export collision | module emits duplicate exported symbol | inspect module exports/build artifacts |
| `VCP023` | import conflict | imported symbol collides with existing fn/struct | rename import/export or alias it |
| `VCP024` | ABI file could not be read | missing/corrupt `.vabi` | rebuild package artifacts |
| `VCP025` | package ABI missing bytecode symbol | ABI expects export not present in bytecode | rebuild package or inspect exports |
| `VCP026` | package ABI metric mismatch | ABI arity/field count differs from bytecode | rebuild package or inspect breaking change |
| `VCP027` | undefined variable | unresolved identifier in expression | inspect local/global/imported symbol scope |
| `VCP028` | undefined variable in assignment | assignment target not declared | declare variable or fix target name |
| `VCP029` | too many call arguments | arg count exceeds bytecode/native limit | reduce arguments or pack into struct/array |
| `VCP030` | unknown namespaced callable | alias.member lookup failed | inspect alias import and exported members |
| `VCP031` | undefined method-call receiver | `alias.member(...)` receiver variable missing | declare receiver or fix alias name |
| `VCP032` | unknown callable | function/native/global resolution failed | inspect symbol name and imports |
| `VCP033` | unsupported call target | attempted dynamic call on unsupported AST target | call a function identifier/get expression instead |
| `VCP034` | invalid spawn target | `spawn` did not receive a call expression | use `spawn fn_name(...)` |
| `VCP035` | unknown spawn callable | spawned function could not resolve | inspect spawn target symbol/import |
| `VCP036` | function prototype missing | registered function proto not found at compile time | inspect prototype registration/import order |
| `VCP037` | struct prototype missing | registered struct proto not found | inspect struct registration/import order |
| `VCP038` | namespace alias too long | `use ... as alias` overflowed buffer | shorten alias |
| `VCP039` | module source open failed | imported module file missing/unreadable | inspect resolved module path |
| `VCP040` | export collection OOM | compiler failed while collecting exports | inspect memory pressure |
| `VCP041` | import visibility application failed | namespace/public export filtering failed | inspect module exports and aliasing |
| `VCP042` | function call arity mismatch | direct function call used wrong arg count | align call-site with function contract |
| `VCP043` | function parameter type mismatch | direct function call violated typed param contract | fix call-site argument types |
| `VCP044` | duplicate ABI params field | repeated `params=` in ABI row | keep a single params field |
| `VCP045` | invalid ABI params field | malformed `params=` contract or count mismatch | regenerate/fix ABI file |

## Async Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VAS001` | epoll init failed | `epoll_create1` failed | inspect kernel support and fd limits |
| `VAS002` | epoll wait failed | `epoll_wait` failed outside `EINTR` | inspect fd lifecycle and OS state |

## Memory Codes

| Code | Meaning | Typical Cause | Typical Fix |
| --- | --- | --- | --- |
| `VME001` | nursery allocation failed | startup allocator could not reserve nursery | inspect system memory / allocator limits |
| `VME002` | heap allocation failed | fallback heap alloc ran out of memory | inspect memory pressure / object growth |

## Supported FFI Type Tokens

- `v` = void return only
- `i` = int32
- `I` = int64
- `f` = float
- `d` = double
- `p` = pointer
- `s` = string pointer
- `{...}` = struct

Examples:

- `s->i`
- `d->d`
- `{i,d}->{i,d}`

## Stability Rule

Once a code is shipped, keep the code stable even if the prose changes.
