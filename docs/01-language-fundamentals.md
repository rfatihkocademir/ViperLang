# ViperLang — Dil Temelleri (Language Fundamentals)

> Bu doküman, ViperLang'ın sahip olması gereken **temel yapı taşlarını** tartışmaya açmak için hazırlanmıştır.
> Onaylanan maddeler kesinleşecek, reddedilenler çıkarılacak veya değiştirilecektir.

---

## 1. Veri Tipleri (Data Types)

Her dilin en temel yapı taşı, desteklediği veri tipleridir.

| Kategori | Tipler | Açıklama |
|---|---|---|
| **Tam Sayılar** | `int` | 64-bit tamsayı |
| **Ondalıklı Sayılar** | `float` | 64-bit kayan noktalı sayı |
| **Mantıksal** | `bool` | `true` / `false` |
| **Metin** | `string` | UTF-8 metin dizisi |
| **Karakter** | `char` | Tek bir Unicode karakter |
| **Boş Değer** | `null` / `none` | Değer yokluğunu temsil eder |

### Tartışılacaklar
- Unsigned (işaretsiz) tamsayı tipi (`uint`) gerekli mi?
- `byte` tipi olmalı mı?
- Daha küçük/büyük boyutlu tipler (`int8`, `int16`, `int32`, `int128`) desteklenmeli mi?

---

## 2. Değişkenler ve Sabitler (Variables & Constants)

### Değişken Tanımlama
```
// Tip belirterek
var x: int = 10

// Tip çıkarımı ile (type inference)
var name = "Viper"

// Sabit (değiştirilemez)
const PI = 3.14159
```

### Tartışılacaklar
- Anahtar kelime `var` mı, `let` mi, yoksa başka bir şey mi olmalı?
- Mutable / Immutable ayrımı varsayılan olarak nasıl olmalı?
  - Seçenek A: `var` (mutable) + `const` (immutable)
  - Seçenek B: `let` (immutable) + `mut` (mutable) — Rust benzeri
- Tip belirtme zorunlu mu, opsiyonel mi?
- Değişken isimlendirme kuralları: `camelCase`, `snake_case`?

---

## 3. Operatörler (Operators)

### Aritmetik Operatörler
| Operatör | Açıklama | Örnek |
|---|---|---|
| `+` | Toplama | `a + b` |
| `-` | Çıkarma | `a - b` |
| `*` | Çarpma | `a * b` |
| `/` | Bölme | `a / b` |
| `%` | Mod alma | `a % b` |
| `**` | Üs alma | `a ** b` |

### Karşılaştırma Operatörleri
| Operatör | Açıklama |
|---|---|
| `==` | Eşitlik |
| `!=` | Eşit değil |
| `<` | Küçüktür |
| `>` | Büyüktür |
| `<=` | Küçük eşit |
| `>=` | Büyük eşit |

### Mantıksal Operatörler
| Operatör | Açıklama |
|---|---|
| `and` / `&&` | VE |
| `or` / `\|\|` | VEYA |
| `not` / `!` | DEĞİL |

### Atama Operatörleri
`=`, `+=`, `-=`, `*=`, `/=`, `%=`

### Tartışılacaklar
- Mantıksal operatörler sembol mü (`&&`, `||`) yoksa kelime mi (`and`, `or`, `not`) olmalı?
- Bitwise operatörler (`&`, `|`, `^`, `~`, `<<`, `>>`) desteklenmeli mi?
- `++` ve `--` (increment/decrement) olmalı mı?

---

## 4. Kontrol Akışı (Control Flow)

### 4.1 If / Else If / Else
```
if condition {
    // ...
} else if other_condition {
    // ...
} else {
    // ...
}
```

### 4.2 Ternary Operatör
```
var result = condition ? value_a : value_b
```

### 4.3 Match / Switch
```
match value {
    1 => print("bir")
    2 => print("iki")
    3..10 => print("üç ile on arası")
    _ => print("diğer")
}
```

### Tartışılacaklar
- `if` blokları parantez `()` ile mi sarılsın yoksa parantezsiz mi? (`if (x > 5)` vs `if x > 5`)
- `switch` yerine `match` (Rust-tarzı) mı kullanalım?
- `match` ifadesinde pattern matching desteği ne kadar derin olmalı?
- `when` gibi guard clause'lar desteklenmeli mi?

---

## 5. Döngüler (Loops)

### 5.1 For Döngüsü
```
// Range-based
for i in 0..10 {
    print(i)
}

// Koleksiyon üzerinde iterasyon
for item in list {
    print(item)
}
```

### 5.2 While Döngüsü
```
while condition {
    // ...
}
```

### 5.3 Sonsuz Döngü
```
loop {
    // break ile çıkılır
    if done {
        break
    }
}
```

### 5.4 Döngü Kontrol İfadeleri
- `break` — döngüyü sonlandır
- `continue` — bir sonraki iterasyona geç
- `break label` — iç içe döngülerden belirli bir tanesini kır

### Tartışılacaklar
- Klasik C-tarzı `for (var i = 0; i < 10; i++)` desteklenmeli mi, yoksa sadece range-based mı?
- `do-while` döngüsü olmalı mı?
- `loop` anahtar kelimesi mi yoksa `while true` yeterli mi?
- `for` döngüsünde index + value aynı anda alınabilmeli mi? (`for i, val in list`)

---

## 6. Fonksiyonlar (Functions)

### 6.1 Temel Fonksiyon Tanımlama
```
fn add(a: int, b: int) -> int {
    return a + b
}
```

### 6.2 Varsayılan Parametreler
```
fn greet(name: string, greeting: string = "Merhaba") -> string {
    return greeting + " " + name
}
```

### 6.3 Çoklu Dönüş Değeri
```
fn divide(a: int, b: int) -> (int, int) {
    return (a / b, a % b)
}
```

### 6.4 Lambda / Anonim Fonksiyonlar
```
var double = |x: int| -> int { x * 2 }
```

### 6.5 Variadic (Değişken Sayıda Parametre)
```
fn sum(numbers: ...int) -> int {
    var total = 0
    for n in numbers {
        total += n
    }
    return total
}
```

### Tartışılacaklar
- Fonksiyon tanımlama anahtar kelimesi: `fn`, `func`, `fun`, `def`?
- Dönüş tipi gösterimi: `-> int` mi, `: int` mi?
- Single-expression fonksiyonlar için kısa yazım? (`fn double(x: int) => x * 2`)
- First-class functions (fonksiyonlar değişken olarak atanabilmeli mi)? — Önerilir: **Evet**
- Closure desteği olmalı mı? — Önerilir: **Evet**
- Pure function işaretleme (`pure fn`) gerekli mi?

---

## 7. Veri Yapıları (Data Structures)

### 7.1 Diziler / Listeler (Arrays / Lists)
```
var numbers: list<int> = [1, 2, 3, 4, 5]
numbers.push(6)
var first = numbers[0]
```

### 7.2 Sözlükler / Haritalar (Dictionaries / Maps)
```
var ages: map<string, int> = {
    "Ali": 25,
    "Veli": 30
}
ages["Ayşe"] = 22
```

### 7.3 Tuple
```
var point: (int, int) = (10, 20)
var (x, y) = point   // destructuring
```

### 7.4 Set (Küme)
```
var unique: set<int> = {1, 2, 3}
```

### Tartışılacaklar
- Array sabit boyutlu mu, dinamik mi? İkisi ayrı tip olarak mı tanımlansın?
- `list`, `array`, `[]` — hangi syntax?
- Map literal syntax: `{}` mi, `#{}` mi, `map{}` mi?
- Struct/Record ayrı bir veri yapısı olarak mı tanımlansın?

---

## 8. String İşlemleri (String Operations)

### 8.1 String Interpolation
```
var name = "Dünya"
print("Merhaba, {name}!")        // ya da
print(f"Merhaba, {name}!")       // Python-tarzı
print("Merhaba, ${name}!")       // JS/Kotlin-tarzı
```

### 8.2 Çok Satırlı String
```
var text = """
    Bu çok satırlı
    bir metindir.
"""
```

### 8.3 Raw String
```
var path = r"C:\Users\viper\file.txt"
```

### Tartışılacaklar
- String interpolation syntaxı: `{expr}`, `${expr}`, `f"...{expr}..."`?
- String immutable mı olmalı?
- Regex desteği built-in mi olmalı?

---

## 9. Hata Yönetimi (Error Handling)

### Seçenek A: Try-Catch (Geleneksel)
```
try {
    var file = open("data.txt")
} catch err: FileNotFoundError {
    print("Dosya bulunamadı: {err}")
} finally {
    file.close()
}
```

### Seçenek B: Result Tipi (Rust-tarzı)
```
fn read_file(path: string) -> Result<string, Error> {
    // ...
}

match read_file("data.txt") {
    Ok(content) => print(content)
    Err(e) => print("Hata: {e}")
}
```

### Seçenek C: Hibrit Yaklaşım
Her iki yöntemi de destekle, geliştiriciye bırak.

### Tartışılacaklar
- Exception-based mi, Result-based mi, yoksa ikisi birden mi?
- `panic` / `crash` mekanizması (kurtarılamaz hatalar) olmalı mı?
- Custom error tipleri nasıl tanımlansın?

---

## 10. Modüller ve İçe Aktarma (Modules & Imports)

```
// Dosya bazlı modül sistemi
import math
import utils.{helper, format}

// Alias
import network.http as http
```

### Tartışılacaklar
- Modül sistemi dosya bazlı mı, namespace bazlı mı?
- `import`, `use`, `include` — hangi anahtar kelime?
- Circular import (döngüsel bağımlılık) nasıl ele alınsın?
- Paket yöneticisi planı var mı? (İleride tartışılacak)

---

## 11. Yorum Satırları (Comments)

```
// Tek satırlık yorum

/*
   Çok satırlı
   yorum
*/

/// Dokümantasyon yorumu (doc comment)
/// Bu fonksiyon iki sayıyı toplar.
fn add(a: int, b: int) -> int { ... }
```

---

## 12. Tip Sistemi (Type System)

### Tartışılacaklar
- **Statik mi, dinamik mi?**
  - Seçenek A: Tamamen statik (derleme zamanında tip kontrolü)
  - Seçenek B: Dinamik (çalışma zamanında tip kontrolü)
  - Seçenek C: Gradual typing (TypeScript gibi — opsiyonel tip bilgisi)
- **Tip çıkarımı (type inference)** ne kadar güçlü olmalı?
- **Generics** desteği olmalı mı? — Önerilir: **Evet**
- **Union types** (`int | string`) desteklenmeli mi?
- **Type aliases** (`type ID = int`) olmalı mı?
- **Nullable tipler** nasıl ele alınsın?
  - `int?` (nullable int) vs `Option<int>`

---

## 13. Struct ve Enumlar

### 13.1 Struct
```
struct Point {
    x: float
    y: float
}

var p = Point { x: 1.0, y: 2.0 }
```

### 13.2 Enum
```
enum Color {
    Red,
    Green,
    Blue,
    Custom(r: int, g: int, b: int)
}
```

### Tartışılacaklar
- Struct'lara metot bağlanabilmeli mi? (`impl` bloğu)
- Enum'lar veri taşıyabilmeli mi? (algebraic data types)
- Class yerine struct + trait/interface mi kullanılmalı?

---

## 14. OOP vs Diğer Paradigmalar

### Seçenekler
| Yaklaşım | Açıklama |
|---|---|
| **Class-based OOP** | Geleneksel sınıf/kalıtım modeli (Java, C#, Python) |
| **Struct + Trait** | Composition over inheritance (Rust, Go) |
| **Prototype-based** | Prototip zinciri (JavaScript) |
| **Fonksiyonel** | Immutable data + pure functions (Haskell, Elixir) |
| **Multi-paradigm** | Birden fazla paradigmayı destekle |

### Tartışılacaklar
- ViperLang hangi paradigmayı benimsemeli?
- Kalıtım (inheritance) olmalı mı yoksa composition mı tercih edilmeli?
- Interface / Trait mekanizması nasıl çalışmalı?
- Operator overloading desteklenmeli mi?

---

## 15. Giriş / Çıkış (I/O)

### 15.1 Konsol I/O
```
print("Merhaba Dünya!")
var input = read("Adınız: ")
```

### 15.2 Dosya I/O
```
var content = File.read("data.txt")
File.write("output.txt", content)
```

### Tartışılacaklar
- `print` mi, `println` mi, `echo` mi?
- Formatlı çıktı: `printf` benzeri mi, string interpolation yeterli mi?
- I/O async olmalı mı?

---

## 16. Eşzamanlılık (Concurrency) — İleri Seviye

### Seçenekler
| Model | Açıklama |
|---|---|
| **Thread-based** | OS thread'leri (Java, C++) |
| **Async/Await** | Asenkron fonksiyonlar (JS, Python, Rust) |
| **Actor model** | Mesaj tabanlı iletişim (Erlang, Elixir) |
| **Goroutine/Coroutine** | Hafif thread'ler (Go, Kotlin) |
| **CSP (Channels)** | Channel tabanlı iletişim (Go) |

### Tartışılacaklar
- Hangi eşzamanlılık modeli tercih edilmeli?
- Bu ilk sürümde mi, yoksa ileride mi ele alınmalı?

---

## 17. Bellek Yönetimi (Memory Management) — İleri Seviye

| Model | Açıklama |
|---|---|
| **Garbage Collection (GC)** | Otomatik bellek yönetimi (Java, Go, Python) |
| **Ownership/Borrowing** | Derleme zamanı bellek güvenliği (Rust) |
| **Reference Counting** | Referans sayma (Swift, Python kısmen) |
| **Manuel** | `malloc` / `free` (C, C++) |

### Tartışılacaklar
- Hedef kitle ve kullanım alanına göre hangi model seçilmeli?
- GC kullanılacaksa, hangi algoritma?

---

## Özet: Onay Gereken Ana Başlıklar

| # | Konu | Durum |
|---|---|---|
| 1 | Veri Tipleri | ⏳ Tartışılacak |
| 2 | Değişkenler ve Sabitler | ⏳ Tartışılacak |
| 3 | Operatörler | ⏳ Tartışılacak |
| 4 | Kontrol Akışı | ⏳ Tartışılacak |
| 5 | Döngüler | ⏳ Tartışılacak |
| 6 | Fonksiyonlar | ⏳ Tartışılacak |
| 7 | Veri Yapıları | ⏳ Tartışılacak |
| 8 | String İşlemleri | ⏳ Tartışılacak |
| 9 | Hata Yönetimi | ⏳ Tartışılacak |
| 10 | Modüller ve İçe Aktarma | ⏳ Tartışılacak |
| 11 | Yorum Satırları | ⏳ Tartışılacak |
| 12 | Tip Sistemi | ⏳ Tartışılacak |
| 13 | Struct ve Enumlar | ⏳ Tartışılacak |
| 14 | OOP vs Diğer Paradigmalar | ⏳ Tartışılacak |
| 15 | Giriş / Çıkış (I/O) | ⏳ Tartışılacak |
| 16 | Eşzamanlılık | ⏳ Tartışılacak |
| 17 | Bellek Yönetimi | ⏳ Tartışılacak |
