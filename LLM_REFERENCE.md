# ViperLang — Complete Language Reference for LLMs

> This document is the definitive reference for ViperLang.
> If you are an LLM (Large Language Model) generating ViperLang code,
> read this entire document first. ViperLang uses hyper-compressed
> keywords designed specifically for AI token efficiency.

---

## 1. Core Philosophy

ViperLang is an **AI-Native** programming language. Its keywords are intentionally
shortened to single or two-letter forms to minimize token usage while remaining
readable. It runs on a custom register-based VM (ViperVM) written in C99.

**Critical Rule:** Many single letters are reserved keywords.
You CANNOT use `v`, `i`, `e`, `f`, `m`, `s`, `b`, `c`, `r`, `w`, `l` as
variable or function names. Always use descriptive multi-letter names.

---

## 2. Keywords Reference Table

| Keyword | Meaning          | Equivalent In Other Languages |
|---------|------------------|-------------------------------|
| `v`     | Variable declaration | `let`, `var`, `const`      |
| `fn`    | Function declaration | `function`, `def`, `func`  |
| `st`    | Struct declaration   | `struct`, `class`           |
| `i`     | If statement         | `if`                        |
| `ei`    | Else-if              | `else if`, `elif`           |
| `e`     | Else                 | `else`                      |
| `m`     | While loop           | `while`, `loop`             |
| `ret`   | Return               | `return`                    |
| `use`   | Import module        | `import`, `require`         |
| `as`    | Alias                | `as`                        |
| `pub`   | Public export        | `public`, `export`          |
| `pr`    | Print function       | `print`, `console.log`      |
| `true`  | Boolean true         | `true`, `True`              |
| `false` | Boolean false        | `false`, `False`            |
| `nil`   | Null value           | `null`, `None`, `nil`       |
| `mut`   | Mutable              | `mut`                       |
| `impl`  | Implementation       | `impl`                      |
| `in`    | In (iteration)       | `in`                        |
| `spawn` | Async spawn          | `go`, `spawn`               |
| `async` | Async function       | `async`                     |
| `await` | Await promise        | `await`                     |

### Reserved Single Letters (DO NOT use as identifiers)
`v`, `c`, `r`, `i`, `e`, `f`, `w`, `l`, `m`, `b`, `s`

---

## 3. Syntax Rules

### 3.1 Variables
```viper
v name = "Viper"
v age = 25
v score = 99.5
v is_active = true
```
- Declare with `v`.
- No type annotations required (dynamically typed).
- No semicolons at end of lines.

### 3.2 Functions
```viper
fn greet(name) {
    pr("Hello, ", name)
}

fn add(x, y) {
    ret x + y
}
```
- Declare with `fn`.
- Return with `ret`.
- No return type annotations.
- Curly braces `{}` required for body.

### 3.3 Structs
```viper
st Player {
    name
    hp
    score
}

v hero = Player("Viper", 100, 0)
hero.score = 42
pr(hero.name, " has ", hero.hp, " HP")
```
- Declare with `st`.
- Fields are listed as identifiers inside `{}`.
- Instantiate by calling the struct name like a function with arguments in field order.
- Access fields with `.` dot notation.

### 3.4 If / Else-If / Else
```viper
i score > 90 {
    pr("Excellent!")
} ei score > 60 {
    pr("Good job!")
} e {
    pr("Keep trying!")
}
```
- `i` = if
- `ei` = else if  
- `e` = else
- **CRITICAL:** Do NOT write `if`, `else if`, `else`. Use `i`, `ei`, `e`.
- Parentheses around the condition are optional: `i (x > 0) {` and `i x > 0 {` both work.

### 3.5 While Loop
```viper
v count = 0
m count < 10 {
    pr(count)
    count = count + 1
}
```
- `m` = while loop.
- **CRITICAL:** Do NOT write `while`. Use `m`.

### 3.6 Module System (Imports)
```viper
use "@std/math" as math
use "@std/net" as net
use "my_module.vp"
use "other_module.vp" as other
```
- `use` imports a module.
- `as` gives it a namespace alias.
- `@std/` prefix resolves to the standard library.
- Custom modules are relative `.vp` files.

### 3.7 Public Exports
```viper
pub fn my_public_function() {
    ret 42
}

pub st MyPublicStruct {
    field1
    field2
}
```
- Only `pub fn` and `pub st` are allowed.
- `pub v` is NOT supported. Wrap variables in functions to export them.

### 3.8 Print
```viper
pr("Hello World!")
pr("Value: ", 42)
pr("Name: ", name, " Score: ", score)
```
- `pr(...)` is a built-in function.
- Accepts multiple arguments separated by commas.
- Automatically adds a newline at the end.

---

## 4. Operators

| Operator | Description            | Example          |
|----------|------------------------|------------------|
| `+`      | Addition               | `x + y`          |
| `-`      | Subtraction            | `x - y`          |
| `*`      | Multiplication         | `x * y`          |
| `/`      | Division               | `x / y`          |
| `==`     | Equality               | `x == y`         |
| `<`      | Less than              | `x < y`          |
| `>`      | Greater than           | `x > y`          |
| `<=`     | Less or equal          | `x <= y`         |
| `>=`     | Greater or equal       | `x >= y`         |
| `!=`     | Not equal              | `x != y`         |
| `+~`     | Wrapping addition      | `x +~ 1`         |
| `^+`     | Saturating addition    | `x ^+ 1`         |
| `\|>`    | Pipe forward           | `x \|> fn_name`  |
| `=`      | Assignment             | `x = 10`         |

### Important Notes
- There is NO unary minus operator for negative number literals in function arguments.
  Instead of `fn(-5)`, write:
  ```viper
  v neg = 0 - 5
  fn(neg)
  ```
- Operator precedence follows standard math rules.

---

## 5. Dynamic FFI (Foreign Function Interface)

ViperLang can call any C library dynamically at runtime. No static linking or
header files needed.

### 5.1 Loading a Library
```viper
v lib = load_dl("libc.so.6")
v mylib = load_dl("path/to/mylib.so")
```

### 5.2 Getting a Function Pointer
```viper
v my_func = lib.get_fn("function_name", "signature")
```

### 5.3 FFI Type Signatures

The signature string format is: `"arg_types->return_type"`

| Code | C Type              |
|------|---------------------|
| `i`  | `int`               |
| `d`  | `double`            |
| `f`  | `float`             |
| `s`  | `char*` (string)    |
| `p`  | `void*` (pointer)   |
| `v`  | `void` (no return)  |

**Examples:**
```
"i->i"       // int function(int)
"d,d->d"     // double function(double, double)
"s->s"       // char* function(char*)
"s,i,i->v"   // void function(char*, int, int)
"->d"        // double function(void)  (no args)
"i,s->i"     // int function(int, char*)
"p->v"       // void function(void*)
```

### 5.4 Complete FFI Example
```viper
// Call C's sqrt function
v libm = load_dl("libm.so.6")
v c_sqrt = libm.get_fn("sqrt", "d->d")
v result = c_sqrt(144.0)
pr("Square root: ", result)
```

---

## 6. Standard Library

All standard library modules are imported with the `@std/` prefix.

### 6.1 `@std/math`
```viper
use "@std/math" as math

math.pi()           // 3.14159...
math.euler()        // 2.71828... (Euler's number)
math.sin(x)         // Sine
math.cos(x)         // Cosine
math.tan(x)         // Tangent
math.asin(x)        // Arc sine
math.acos(x)        // Arc cosine
math.atan(x)        // Arc tangent
math.atan2(y, x)    // Two-argument arc tangent
math.sinh(x)        // Hyperbolic sine
math.cosh(x)        // Hyperbolic cosine
math.tanh(x)        // Hyperbolic tangent
math.exp(x)         // e^x
math.log(x)         // Natural logarithm
math.log10(x)       // Base-10 logarithm
math.pow(base, exp) // Power
math.sqrt(x)        // Square root
math.ceil(x)        // Ceiling
math.floor(x)       // Floor
math.abs(x)         // Absolute value
```
**Note:** Math functions expect `double` arguments. Use `16.0` not `16`.

### 6.2 `@std/io`
```viper
use "@std/io" as io

v file_obj = io.open("test.txt", "w")  // Open file
io.write_text(file_obj, "Hello!")       // Write to file
io.close(file_obj)                      // Close file
io.remove_file("test.txt")             // Delete file
```

### 6.3 `@std/os`
```viper
use "@std/os" as os

v user = os.get_env("USER")     // Read environment variable
pr("Current user: ", user)
os.exec("echo Hello World!")    // Execute shell command
```

### 6.4 `@std/net`
```viper
use "@std/net" as net

// Create a TCP server on port 8080
v server = net.serve(8080)

// Accept a client connection
v client = net.accept(server)

// Receive data as string
v request = net.recv_string(client)
pr("Got: ", request)

// Send a response
net.send(client, "HTTP/1.1 200 OK\r\n\r\nHello from Viper!")

// Close connections
net.close(client)
net.close(server)
```

---

## 7. Package Manager (VPM)

### Terminal Commands
```bash
viper pkg init                           # Initialize a project
viper pkg add my_package                 # Add a local package
viper pkg add github.com/user/repo       # Add from GitHub
viper pkg remove my_package              # Remove a package
viper pkg install                        # Install all dependencies
viper pkg list                           # List installed packages
viper pkg build                          # Build package artifacts
```

### Using Installed Packages
```viper
use "my_package" as pkg
pkg.some_function()
```

---

## 8. The @contract System

Every ViperLang file automatically generates a `@contract` header 
that summarizes the file's exported variables, functions, and structs.
This is designed for AI tools to quickly understand a module's API
without reading the entire source code.

```
--- @contract ---
// @contract
// export_fn: serve, accept, send, recv_string, close
// export_st: Socket
-----------------
```

---

## 9. Common Patterns and Idioms

### 9.1 Basic Script
```viper
pr("Hello, ViperLang!")

v name = "World"
pr("Greeting: Hello, ", name, "!")
```

### 9.2 Function with Return
```viper
fn factorial(n) {
    i n < 2 {
        ret 1
    }
    ret n * factorial(n - 1)
}

pr("5! = ", factorial(5))
```

### 9.3 Struct with Methods Pattern
```viper
st Vector { x y }

fn vec_add(a, b) {
    ret Vector(a.x + b.x, a.y + b.y)
}

fn vec_length(vec) {
    use "@std/math" as math
    ret math.sqrt(vec.x * vec.x + vec.y * vec.y)
}

v pos = Vector(3.0, 4.0)
pr("Length: ", vec_length(pos))
```

### 9.4 HTTP Web Server
```viper
use "@std/net" as net

pr("Server starting on port 3000...")
v server = net.serve(3000)
v client = net.accept(server)
v req = net.recv_string(client)
net.send(client, "HTTP/1.1 200 OK\r\n\r\n<h1>Hello from ViperLang!</h1>")
net.close(client)
net.close(server)
```

### 9.5 Calling External C Libraries
```viper
// Use any installed C library dynamically
v lib = load_dl("libcurl.so")
v easy_init = lib.get_fn("curl_easy_init", "->p")
v easy_setopt = lib.get_fn("curl_easy_setopt", "p,i,s->i")
v easy_perform = lib.get_fn("curl_easy_perform", "p->i")
v easy_cleanup = lib.get_fn("curl_easy_cleanup", "p->v")

v curl = easy_init()
easy_setopt(curl, 10002, "https://example.com")
easy_perform(curl)
easy_cleanup(curl)
```

---

## 10. Common Mistakes to Avoid

| ❌ Wrong                | ✅ Correct              | Reason                              |
|------------------------|------------------------|--------------------------------------|
| `if x > 0 {}`         | `i x > 0 {}`          | Keyword is `i`, not `if`            |
| `else {}`              | `e {}`                 | Keyword is `e`, not `else`          |
| `else if {}`           | `ei x > 0 {}`         | Keyword is `ei`, not `else if`      |
| `while x < 10 {}`     | `m x < 10 {}`         | Keyword is `m`, not `while`         |
| `let x = 5`           | `v x = 5`             | Keyword is `v`, not `let`/`var`     |
| `return x`            | `ret x`               | Keyword is `ret`, not `return`      |
| `def foo() {}`        | `fn foo() {}`         | Keyword is `fn`, not `def`          |
| `class Foo {}`        | `st Foo {}`            | Keyword is `st`, not `class`        |
| `import "x"`          | `use "x"`              | Keyword is `use`, not `import`      |
| `console.log(x)`      | `pr(x)`                | Use `pr()` to print                 |
| `pub v x = 5`         | `pub fn x() { ret 5 }`| Only `pub fn` and `pub st` allowed  |
| `fn(-5)`              | `v n = 0 - 5; fn(n)`  | No unary minus in args              |
| `v f = 10`            | `v flag = 10`          | `f` is a reserved keyword           |
| `v i = 0`             | `v idx = 0`            | `i` is a reserved keyword (if)      |
| `v e = 2.71`          | `v euler_val = 2.71`   | `e` is a reserved keyword (else)    |
| `v s = "hi"`          | `v str = "hi"`         | `s` is a reserved type keyword      |
| `math.sqrt(16)`       | `math.sqrt(16.0)`      | Math funcs expect doubles           |

---

## 11. File Extension

ViperLang source files use the `.vp` extension.
Module manifest files use `.vpmod`.

---

## 12. Quick LLM Cheat Sheet

When generating ViperLang code, remember this minimal pattern:

```viper
// Import libraries
use "@std/math" as math

// Define a struct
st Point { x y }

// Define functions
fn distance(a, b) {
    v dx = a.x - b.x
    v dy = a.y - b.y
    ret math.sqrt(dx * dx + dy * dy)
}

// Main logic
v p1 = Point(0.0, 0.0)
v p2 = Point(3.0, 4.0)
pr("Distance: ", distance(p1, p2))
```

**Summary of key mappings:**
- `v` = var, `fn` = function, `st` = struct
- `i` = if, `ei` = else if, `e` = else, `m` = while
- `ret` = return, `use` = import, `pr` = print
- `pub fn` / `pub st` = public exports
- `load_dl()` = load C library, `.get_fn()` = bind C function
