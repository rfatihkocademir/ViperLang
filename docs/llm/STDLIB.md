# ViperLang Standard Library For LLMs

This file documents the stdlib modules that actually exist in the repository today.

Rule:

`Prefer stdlib wrappers before raw natives or ad-hoc load_dl calls.`

## Module Map

### `@std/os`

Use for operating-system interaction.

Common functions:

- `os.sh(command)`
- `os.exec(command)`
- `os.get_env(key)`
- `os.setenv(key, val)`
- `os.args()`
- `os.cwd()`
- `os.pid()`
- `os.kill(target_pid, signal_no)`
- `os.sleep(ms)`
- `os.info()`
- `os.cron(spec, callback_fn)`

Typical effect labels:

- `os`
- `async` if cron/scheduling behavior is central to the contract

### `@std/fs`

Use for path and file operations without manually managing file handles.

Common functions:

- `fs.read(path)`
- `fs.read_bytes(path)`
- `fs.write(path, content)`
- `fs.append(path, content)`
- `fs.delete(path)`
- `fs.copy(src, dest)`
- `fs.move(src, dest)`
- `fs.exists(path)`
- `fs.is_dir(path)`
- `fs.mkdir(path)`
- `fs.ls(path)`
- `fs.watch(path, on_change_fn)`
- `fs.stream_read(path, on_chunk_fn)`

Typical effect labels:

- `fs`
- `async` if watch/stream callbacks are central

### `@std/io`

Use for lower-level file handle control.

Types and functions:

- `st File`
- `io.open(path, mode)` returns `File` or `0` if the open fails
- `io.close(file)`
- `io.read_text(file)`
- `io.write_text(file, text)`
- `io.remove_file(path)`

Typical effect labels:

- `fs`
- `ffi` is usually hidden by the stdlib wrapper, but raw FFI should still stay isolated

Guideline:

- Prefer `@std/fs` unless the code really needs an explicit file handle.

### `@std/net`

Use for lower-level sockets and manual request/response loops.

Types and functions:

- `st Socket`
- `net.serve(port)`
- `net.accept(server)`
- `net.send(socket, message)`
- `net.recv_string(socket)`
- `net.http_respond(socket, html)`
- `net.close(socket)`
- `net.accept_async(server)`
- `net.send_async(socket, message)`
- `net.http_respond_file(socket, filepath)`
- `net.event_loop_init()`

Typical effect labels:

- `web`
- `async` for async accept/send flows

Guideline:

- Prefer `@std/web` for app-facing HTTP APIs.
- Use `@std/net` when the LLM needs explicit socket lifecycle control.

### `@std/web`

Use for higher-level HTTP and web-facing code.

Common functions:

- `web.serve(port, handler_fn)`
- `web.route(path, method, handler_fn)`
- `web.middleware(handler_fn)`
- `web.static(path, dir_path)`
- `web.cors(options)`
- `web.fetch(url)`
- `web.post(url, data)`
- `web.put(url, data)`
- `web.patch(url, data)`
- `web.delete(url)`
- `web.ws_serve(port, on_msg_fn)`
- `web.ws_send(client_id, msg)`
- `web.download(url, file_path)`
- `web.upload(url, file_path)`
- `web.jwt_sign(payload, secret)`
- `web.jwt_verify(token, secret)`
- `web.hash(input, algo)`

Typical effect labels:

- `web`

### `@std/db`

Use for database-backed workflows.

Common functions:

- `db.connect(conn_str)`
- `db.query(sql_text)`
- `db.get(table, id)`
- `db.find(table, where_text)`
- `db.first(table, where_text)`
- `db.save(table, data_text)`
- `db.insert(table, data_text)`
- `db.update(table, id, data_text)`
- `db.upsert(table, conflict_key, data_text)`
- `db.delete(table, id)`
- `db.begin()`
- `db.commit()`
- `db.rollback()`
- `db.count(table, where_text)`
- `db.exists(table, where_text)`
- `db.paginate(table, where_text, page, size)`
- `db.sync(table, schema)`

Typical effect labels:

- `db`

Guideline:

- Keep query/transaction code in small helpers.
- Split DB access from formatting/serialization helpers.

### `@std/cache`

Use for cache-centric logic.

Common functions:

- `cache.set(key, value, ttl_ms)`
- `cache.get(key)`
- `cache.delete(key)`
- `cache.has(key)`
- `cache.increment(key, amount)`
- `cache.clear()`
- `cache.keys(pattern)`

Typical effect labels:

- `cache`

### `@std/ai`

Use for provider-backed AI calls.

Common functions:

- `ai.config(provider, key)`
- `ai.ask(prompt)`
- `ai.chat(messages)`
- `ai.embed(text)`
- `ai.extract(text, schema)`
- `ai.vision(image_path, prompt)`
- `ai.tool(fn_reference)`

Typical effect labels:

- `ai`

Guideline:

- Keep prompt construction and response normalization in separate pure helpers.

### `@std/meta`

Use for semantic/runtime introspection.

Common functions:

- `meta.symbols()`
- `meta.ast(path)`
- `meta.eval_sandboxed(code, permissions)`
- `meta.test_runner(dir_path)`
- `meta.compress_context()`

Typical effect labels:

- `meta`
- `dynamic` when `eval_sandboxed` meaningfully changes behavior

### `@std/time`

Use for timestamps and formatting.

Common functions:

- `time.now()`
- `time.format(timestamp, pattern)`
- `time.parse(date_text, pattern)`
- `time.add(timestamp, spec)`

Guideline:

- Time helpers are often utility-only; most functions using them do not need a public effect label unless they also do IO or system interaction.

### `@std/math`

Use for numeric helpers.

Common functions:

- `math.pi()`
- `math.euler()`
- `math.sin(x)`
- `math.cos(x)`
- `math.tan(x)`
- `math.pow(base, exponent)`
- `math.sqrt(x)`
- `math.ceil(x)`
- `math.floor(x)`
- `math.abs(x)`
- `math.rand(min, max)`
- `math.uuid()`
- `math.round(num, decimals)`
- `math.hash(data)`

Guideline:

- Treat math-heavy helpers as pure unless they also touch stateful modules.

### `@std/crypto`

Use for hashing and HMAC helpers.

Common functions:

- `crypto.hmac_sha256(key, data)`
- `crypto.sha256(data)`

Guideline:

- Keep crypto helpers narrow and deterministic.

## LLM Authoring Rules

- Import only the modules the file actually uses.
- Prefer one side-effect domain per helper when possible.
- If a function does both IO and formatting, split it.
- If a high-level module already exists, prefer it over direct `load_dl(...)`.
