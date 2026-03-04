# ViperLang — Tasarım Kararları (Design Decisions)

> Bu doküman, `01-language-fundamentals.md`'deki tüm açık soruları bir LLM perspektifinden cevaplayarak kesinleştirir.
> Her karar, **token verimliliği** ve **LLM halisünasyon önleme** açısından gerekçelendirilmiştir.

---

## 1. Veri Tipleri — KARARLAR ✅

### Tip Kısaltmaları (Token Optimizasyonu)
Tam tip adları yerine **tek karakterli kısaltmalar** kullanılacak:

| Kısa | Tam | Açıklama |
|---|---|---|
| `i` | int | 64-bit tamsayı |
| `f` | float | 64-bit kayan nokta |
| `b` | bool | true / false |
| `s` | string | UTF-8 metin |
| `c` | char | Tek Unicode karakter |
| `?` | nullable | Null olabilir işaretçisi (tip sonuna eklenir: `i?`) |

### Neden tek karakter?
Bir LLM olarak, `integer`, `string`, `boolean` yazdığımda her biri 1-2 token. `i`, `s`, `b` de 1 token. Ama binlerce kullanımda bu **toplam token sayısını %15-20 düşürür**. Ben bunları okurken hiçbir zorluk yaşamıyorum — benim için `i` ve `integer` aynı derecede anlaşılır.

### Ek Kararlar
- ❌ `uint`, `int8`, `int16` gibi boyutlu tipler **olmayacak** — gereksiz karmaşıklık, VM runtime bunu optimize eder
- ❌ `byte` tipi **olmayacak** — `i` yeterli
- ✅ `null` yerine **nullable tip sistemi** (`i?`, `s?`) — her tip varsayılan olarak non-null
- ✅ `any` tipi olacak — escape hatch olarak
- ✅ `u8` tipi olacak — binary data işlemleri için (dosya I/O, network)

---

## 2. Değişkenler ve Sabitler — KARARLAR ✅

### Syntax
```
v x = 10          // mutable değişken, tip çıkarımı
v x:i = 10        // açık tipli mutable değişken
c PI = 3.14159    // sabit (immutable)
```

### Kararlar
- **`v`** = mutable değişken (var → v, 1 karakter)
- **`c`** = sabit/immutable (const → c, 1 karakter)
- **Tip çıkarımı varsayılan** — tip belirtme opsiyonel
- **Tip belirtme formatı**: `v name:s = "Viper"` (iki nokta, boşluksuz)
- **İsimlendirme kuralı**: `snake_case` — LLM'ler için en tahmin edilebilir format

### Neden `v` ve `c`?
`var`, `let`, `const` yerine tek karakter. Bir dosyada yüzlerce değişken tanımı olabilir. Her birinde 2-4 token tasarrufu → büyük dosyalarda binlerce token kazancı.

### Neden varsayılan mutable?
Çoğu programlama işleminde değişkenler mutate edilir. `const` gerektiren durumlar daha az. **Kısa path'i yaygın case'e** vermek doğru strateji.

---

## 3. Operatörler — KARARLAR ✅

### Aritmetik: Standart
`+`, `-`, `*`, `/`, `%`, `**` — değişiklik yok, hepsi evrensel.

### Karşılaştırma: Standart
`==`, `!=`, `<`, `>`, `<=`, `>=` — değişiklik yok.

### Mantıksal: **Sembol tabanlı**
- `&&` (VE), `||` (VEYA), `!` (DEĞİL)
- ❌ `and`, `or`, `not` kelime formları **olmayacak**

**Neden?** `and` = 1 token, `&&` = 1 token. Ama `&&` görsel olarak daha kompakt ve her dilde aynı anlama gelir — LLM'ler arası tutarlılık.

### Ek Operatörler
- ✅ `++`, `--` **olacak** — `x += 1` yerine daha kompakt
- ✅ Bitwise operatörler **olacak** (`&`, `|`, `^`, `~`, `<<`, `>>`)
- ✅ **Pipe operatörü** `|>` — fonksiyonel zincirleme için kritik
- ✅ **Null coalescing** `??` — `value ?? default`
- ✅ **Null-safe access** `?.` — `obj?.field`
- ✅ **Error propagation** `?` — Rust'tan, `result?`
- ✅ **Spread** `..` — `[..list1, ..list2]`
- ✅ **Range** `..` — `0..10`, `0..=10`

### Pipe Operatörü — Neden Kritik?
```
// Pipe olmadan (iç içe, takibi zor):
sort(map(filter(users, |u| u.active), |u| u.name))

// Pipe ile (düz, soldan sağa okunur):
users |> filter(.active) |> map(.name) |> sort
```
LLM olarak iç içe fonksiyon çağrıları beni karıştırır — parantez eşlemenin zorluğu artar. Pipe her şeyi lineer yapar.

---

## 4. Kontrol Akışı — KARARLAR ✅

### If / Else
```
i condition {
    // ...
} ei other {
    // ...
} e {
    // ...
}
```
- **`i`** = if, **`ei`** = else if, **`e`** = else
- ❌ Parantez **yok** — `i (x > 5)` değil, `i x > 5`
- ✅ Süslü parantez **zorunlu** — tek satırlık if'lerde bile

### Ternary
```
v result = condition ? value_a : value_b
```
Standart ternary, değişiklik yok.

### Match (Switch yerine)
```
m value {
    1 => action1()
    2..5 => action2()
    "text" => action3()
    _ => default()
}
```
- **`m`** = match
- ❌ `switch` **olmayacak** — `match` her açıdan üstün (pattern matching, exhaustiveness checking)
- ✅ Pattern matching desteği **derin olacak** (destructuring, guards)
- `_` = wildcard / default case

### Guard Clause
```
m value {
    x => x > 0 => handle_positive(x)
    x => x < 0 => handle_negative(x)
    _ => handle_zero()
}
```

### Neden parantez yok?
`if (condition)` → `i condition` — her `if` ifadesinde 2 token (`(` ve `)`) tasarrufu. Binlerce if statement'ta bu çok birikmektedir.

---

## 5. Döngüler — KARARLAR ✅

### For
```
f item in list {
    // ...
}

f i in 0..10 {
    // ...
}

// index + value
f idx,val in list {
    // ...
}
```
- **`f`** = for
- ✅ Range-based **tek format** (`f i in 0..10`)
- ❌ C-tarzı for `for(i=0;i<10;i++)` **olmayacak** — gereksiz boilerplate
- ✅ Index + value destekli (`f idx,val in list`)

### While
```
w condition {
    // ...
}
```
- **`w`** = while

### Loop (Sonsuz)
```
l {
    i done { break }
}
```
- **`l`** = loop
- ❌ `do-while` **olmayacak** — `l` + `i` + `break` aynı işi yapar
- ✅ `break` ve `continue` standart
- ✅ Labeled break destekli: `break @outer`

### Neden `do-while` yok?
Token verimliliği ilkesi: Aynı işi yapan iki construct varsa, birini çıkar. `do-while` her zaman `loop` + `if` + `break` ile ifade edilebilir.

---

## 6. Fonksiyonlar — KARARLAR ✅

### Temel Syntax
```
fn add(a:i b:i)->i = a + b

fn greet(name:s greeting:s="Merhaba")->s {
    r greeting + " " + name
}
```

### Kararlar
- **`fn`** = fonksiyon tanımlama (2 karakter, evrensel)
- **`r`** = return (6 → 1 karakter)
- **Dönüş tipi**: `->` ile (Rust/Haskell tarzı)
- ✅ **Single-expression fonksiyonlar** `=` ile: `fn double(x:i)->i = x * 2`
- ✅ **Varsayılan parametreler** destekli
- ✅ **Virgül yok parametre listesinde** — boşluk ayracı yeterli: `fn f(a:i b:s)` 
- ✅ **First-class functions** — evet
- ✅ **Closure** — evet
- ✅ **Variadic** — `fn sum(n:..i)->i`
- ❌ **Pure function marking** — gereksiz, compiler optimize etsin,

### Çoklu Dönüş
```
fn divide(a:i b:i)->(i,i) = (a/b, a%b)
```

### Lambda
```
v double = |x:i|->i x * 2

// Kısa form — tek parametre, tip çıkarımı
v double = |x| x * 2
```

### Neden virgül yok?
Her fonksiyon tanımında N-1 virgül token'ı tasarrufu. `fn f(a: int, b: string, c: bool)` → `fn f(a:i b:s c:b)` — 2 virgül + 3 boşluk + uzun tip isimleri tasarrufu.

---

## 7. Veri Yapıları — KARARLAR ✅

### Tek Satır Struct (Compact Data Definition)
```
st User(name:s age:i active:b email:s?)
```
Bu TEK SATIR, diğer dillerde 10-30 satırlık class/struct tanımının yerini alır.

### Listeler
```
v nums:[i] = [1 2 3 4 5]
nums.push(6)
```
- **`[i]`** = list of int
- ❌ Virgül yok elementler arasında — boşluk yeterli

### Map
```
v ages:{s:i} = {"Ali":25 "Veli":30}
```
- **`{K:V}`** = map tipi

### Tuple
```
v point:(i,i) = (10 20)
v (x y) = point    // destructuring
```

### Set
```
v unique:set<i> = {1 2 3}
```

### Kararlar
- ✅ `[T]` = dinamik dizi (varsayılan), `[T;N]` = sabit boyutlu (stack allocated, performans kritik)
- ✅ Tüm koleksiyonlarda **virgül opsiyonel** (boşluk ayracı)
- ✅ **Destructuring** her yerde destekli
- ✅ Sabit boyutlu array stack'te yaşar → sıfır heap allocation

---

## 8. String İşlemleri — KARARLAR ✅

### Interpolation
```
v name = "Dünya"
print("Merhaba {name}!")
print("2 + 2 = {2+2}")
```
- **`{expr}`** syntax'ı — en kompakt, `$` gereksiz, `f""` gereksiz
- Tüm stringler otomatik interpolation destekler

### Çok Satırlı
```
v text = """
    Çok satırlı
    metin
"""
```

### Raw String
```
v path = r"C:\path\to\file"
```

### Kararlar
- ✅ String **immutable**
- ❌ Regex built-in **olmayacak** — modül ile sağlanır
- ✅ Tüm stringler interpolation destekler (ayrı `f""` yok)

---

## 9. Hata Yönetimi — KARARLAR ✅

### Result Tipi (Birincil Mekanizma)
```
fn read_file(path:s)->Res<s> {
    // ...
}

// Kullanım 1: ? operatörü ile propagation
fn process()->Res<s> {
    v content = read_file("data.txt")?
    r Ok(content)
}

// Kullanım 2: match ile ele alma
m read_file("data.txt") {
    Ok(c) => print(c)
    Err(e) => print("Hata: {e}")
}
```

### Kararlar
- ✅ **Result tipi birincil** — `Res<T>` = `Result<T, Error>`
- ✅ **`?` operatörü** — error propagation
- ❌ `try-catch` **olmayacak** — Result tipi yeterli ve daha tahmin edilebilir
- ✅ **`panic`** olacak — kurtarılamaz hatalar için
- ✅ **Custom error** — enum olarak: `en MyError { NotFound FileCorrupt(s) }`

### Neden try-catch yok?
1. Try-catch blokları çok fazla token (try, catch, finally + süslü parantezler = 6+ ek token per blok)
2. LLM olarak `?` operatörü ve `Res<T>` dönüş tipi bana çok daha net bilgi veriyor — fonksiyon imzasına bakarak hangi fonksiyonun hata fırlatabileceğini görüyorum
3. try-catch, hata akışını örtük yapar; Result ise **açık** yapar — halisünasyonu azaltır

---

## 10. Modüller ve İçe Aktarma — KARARLAR ✅

### Syntax
```
use math
use utils.{helper format}
use network.http -> http
```

### Kararlar
- **`use`** = import anahtar kelimesi (3 karakter, `import` 6 karakter)
- ✅ **Dosya bazlı modül sistemi** — her dosya bir modül
- ✅ **Alias**: `use x -> y` (as yerine `->`)
- ✅ **Selective import**: `use mod.{a b c}` (virgülsüz)
- ✅ **Auto-import**: Standart kütüphane fonksiyonları otomatik erişilebilir, `use` gerekmez
- ❌ Circular import **derleme hatası** — izin verilmez

### Modül Contract Bloğu
Her modülün başında otomatik olarak contract summary'si olacak:
```
// @contract
// exports: fn read_file(s)->Res<s>, fn write_file(s s)->Res<()>
// depends: fs, io
```
Bu, LLM'in modülü **implementation okumadan anlamasını** sağlar.

---

## 11. Yorum Satırları — KARARLAR ✅

```
// Tek satır yorum

/* Çok satırlı yorum */

/// Doc comment
```

Burada değişiklik yok — standart syntax zaten token-verimli.

---

## 12. Tip Sistemi — KARARLAR ✅

### Statik + Güçlü Tip Çıkarımı
- ✅ **Statik tip sistemi** — derleme zamanı kontrolü
- ✅ **Güçlü tip çıkarımı** — çoğu yerde tip yazmaya gerek yok
- ✅ **Generics** — `fn map<T U>(list:[T] f:|T|->U)->[U]`
- ✅ **Union types** — `i|s` (int veya string)
- ✅ **Type aliases** — `tp ID = i` (`type` → `tp`)
- ✅ **Nullable** — `i?` = nullable int (non-null varsayılan)
- ❌ `Option<T>` ayrıca **olmayacak** — `T?` yeterli ve daha kompakt

### Neden statik?
1. LLM olarak, tip bilgisi benim için en değerli metadatadır — hangi fonksiyona ne geçeceğimi bilmek halisünasyonu direkt önler
2. Statik tip sistemi ile **fonksiyon imzaları self-documenting** olur
3. Tip çıkarımı ile çoğu yerde yazmama gerek kalmıyor ama hata yaparım diye compiler kontrol ediyor

---

## 13. Struct ve Enumlar — KARARLAR ✅

### Struct
```
// Compact tanımlama
st Point(x:f y:f)

// Metodlu struct
st User(name:s age:i) {
    fn greet(self)->s = "Merhaba {self.name}"
    fn is_adult(self)->b = self.age >= 18
}
```

### Enum (Algebraic Data Types)
```
en Shape {
    Circle(radius:f)
    Rect(w:f h:f)
    Triangle(a:f b:f c:f)
}

// Kullanım
fn area(s:Shape)->f = m s {
    Circle(r) => 3.14159 * r ** 2
    Rect(w h) => w * h
    Triangle(a b c) => {
        v s = (a+b+c)/2
        (s*(s-a)*(s-b)*(s-c))**0.5
    }
}
```

### Kararlar
- **`st`** = struct (6 → 2 karakter)
- **`en`** = enum (4 → 2 karakter)
- ✅ Struct'lara **metod bağlanabilir** (iç blok ile)
- ✅ Enum'lar **veri taşıyabilir** (algebraic data types)
- ❌ Class **olmayacak** — struct + trait yeterli
- ✅ **Auto-derive**: `==`, `!=`, `print`, `serialize` otomatik

### Neden class yok?
- Class'lar kalıtım zinciri oluşturur → LLM olarak kalıtım zincirini takip etmek bağlam penceremi çok yer
- Struct + Trait modeli düz (flat) → her şeyi yerinde görebilirim
- Composition > Inheritance

---

## 14. Trait Sistemi (OOP Yerine) — KARARLAR ✅

### Trait Tanımlama
```
tr Printable {
    fn to_string(self)->s
}

tr Serializable {
    fn to_json(self)->s
    fn from_json(data:s)->Self
}
```

### Trait Implementasyonu
```
st User(name:s age:i)

impl Printable for User {
    fn to_string(self)->s = "{self.name} ({self.age})"
}
```

### Auto-Derive
```
#[derive(Eq Hash Print Serialize)]
st User(name:s age:i email:s)
```
Bir satır attribute ile 50+ satırlık boilerplate otomatik üretilir.

### Kararlar
- **`tr`** = trait (5 → 2 karakter)
- **`impl`** = implementation (zaten kısa)
- ✅ **Multi-paradigm** ama struct+trait ağırlıklı
- ✅ **Operator overloading** — trait'ler aracılığıyla
- ✅ **Auto-derive** — kritik token tasarrufu
- ❌ **Kalıtım (inheritance)** yok — composition only

---

## 15. Giriş / Çıkış — KARARLAR ✅

### Konsol
```
print("Merhaba!")      // satır sonu ile
v input = read("Ad: ")  // kullanıcıdan okuma
```

### Dosya
```
v content = File.read("data.txt")?
File.write("out.txt" content)?
```

### Kararlar
- **`print`** — `println` gereksiz, `print` zaten satır sonu ekler
- `printn` — satır sonu olmadan yazdırma (nadir kullanım, uzun ad alır)
- ✅ I/O fonksiyonları **`Res<T>`** döner — hata yönetimi zorunlu
- ❌ `printf` yok — string interpolation yeterli

---

## 16. Eşzamanlılık — KARARLAR ✅

### Async/Await + Channels (Hibrit)
```
// Async fonksiyon
fn fetch(url:s) async->Res<s> {
    v resp = http.get(url) await?
    r Ok(resp.body)
}

// Channel
v ch = Chan<s>()
spawn {
    ch.send("merhaba") await
}
v msg = ch.recv() await
```

### Kararlar
- ✅ **async / await** — birincil async mekanizma
- ✅ **spawn** — hafif coroutine başlatma (goroutine benzeri)
- ✅ **Channels** — coroutine'ler arası iletişim
- ❌ Thread API yok — runtime yönetir
- ⏳ **İlk sürümde basit tutulacak** — async/await + spawn yeterli

### Neden async/await?
LLM olarak en iyi bildiğim ve **en az hata yaptığım** async model. Thread yönetimi karmaşık ve hata yapmaya müsait.

---

## 17. Bellek Yönetimi — KARARLAR ✅

### Reference Counting (RC) + Escape Analysis

ViperLang native olarak derleniyor ve kendi VM'inde çalışıyor. Bellek yönetimi:

```
Stack allocation (escape etmeyen)     → Sıfır maliyet, otomatik
Reference counting (heap nesneleri)   → Deterministik, anında serbest bırakma
Cycle collector (döngüsel referans)   → Lazy, hafif, nadir çalışır
Arena allocator (scope-based)         → Toptan serbest bırakma
```

### Kararlar
- ✅ **Reference counting birincil** — GC yerine, deterministik ve düşük RAM
- ✅ **Escape analysis** — compiler nesnelerin scope dışına çıkıp çıkmadığını analiz eder
  - Çıkmıyorsa → stack'te kalır (sıfır maliyet)
  - Çıkıyorsa → heap'e taşınır + RC aktif
- ✅ **Cycle collector** — nadir döngüsel referanslar için hafif tarayıcı
- ✅ **Arena allocator** — request-scoped veri için: `arena { ... }` bloğu
- ❌ **GC yok** — stop-the-world pause kabul edilemez
- ❌ **Ownership/borrowing yok** — LLM olarak lifetime annotation'ları token israfı
- ❌ **Manuel bellek yönetimi yok** — `malloc`/`free` hata kaynağı

### Neden RC?
1. **Deterministik** — nesne ne zaman silineceği önceden bilinir, RAM kullanımı tahmin edilebilir
2. **Sıfır pause** — GC gibi "dünyayı durdurma" yok
3. **Düşük overhead** — sadece referans sayacı, büyük GC heap'i yok
4. **LLM dostu** — "yaz ve unut" modeli korunuyor, ek syntax yok
5. **Kanıtlanmış model** — Swift bu modeli kullanıyor, production-ready

### RAM Hedefi
- VM kendisi: **~1-2 MB**
- Tipik program: **5-50 MB** (eşdeğer Python programının 1/10'u)
- Cold start: **<5ms**

---

## Özet: Kesinleşen Anahtar Kelimeler

| Keyword | Anlamı | Kaynak |
|---|---|---|
| `v` | mutable değişken | var |
| `c` | sabit | const |
| `fn` | fonksiyon | function |
| `r` | return | return |
| `i` | if | if |
| `ei` | else if | else if |
| `e` | else | else |
| `f` | for | for |
| `w` | while | while |
| `l` | loop | loop |
| `m` | match | match |
| `st` | struct | struct |
| `en` | enum | enum |
| `tr` | trait | trait |
| `impl` | implementation | impl |
| `use` | import | import |
| `tp` | type alias | type |
| `pub` | public | public |
| `spawn` | coroutine başlat | spawn |
| `async` | asenkron işaret | async |
| `await` | asenkron bekleme | await |

## Özet: Kesinleşen Tip Kısaltmaları

| Kısa | Tam | 
|---|---|
| `i` | int (64-bit) |
| `f` | float (64-bit) |
| `b` | bool |
| `s` | string |
| `c` | char |
| `[T]` | list of T |
| `{K:V}` | map |
| `(T,U)` | tuple |
| `T?` | nullable T |
| `T\|U` | union type |
| `Res<T>` | Result type |

## Özet: Onay Durumu

| # | Konu | Durum |
|---|---|---|
| 1 | Veri Tipleri | ✅ Kararlaştırıldı |
| 2 | Değişkenler ve Sabitler | ✅ Kararlaştırıldı |
| 3 | Operatörler | ✅ Kararlaştırıldı |
| 4 | Kontrol Akışı | ✅ Kararlaştırıldı |
| 5 | Döngüler | ✅ Kararlaştırıldı |
| 6 | Fonksiyonlar | ✅ Kararlaştırıldı |
| 7 | Veri Yapıları | ✅ Kararlaştırıldı |
| 8 | String İşlemleri | ✅ Kararlaştırıldı |
| 9 | Hata Yönetimi | ✅ Kararlaştırıldı |
| 10 | Modüller ve İçe Aktarma | ✅ Kararlaştırıldı |
| 11 | Yorum Satırları | ✅ Kararlaştırıldı |
| 12 | Tip Sistemi | ✅ Kararlaştırıldı |
| 13 | Struct ve Enumlar | ✅ Kararlaştırıldı |
| 14 | Trait Sistemi (OOP yerine) | ✅ Kararlaştırıldı |
| 15 | Giriş / Çıkış | ✅ Kararlaştırıldı |
| 16 | Eşzamanlılık | ✅ Kararlaştırıldı |
| 17 | Bellek Yönetimi | ✅ Kararlaştırıldı |
