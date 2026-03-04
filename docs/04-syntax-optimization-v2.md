# ViperLang — Syntax Optimizasyonu v2

> Mevcut syntax'ı **her token açısından** tekrar değerlendirdim.
> Bu doküman, daha fazla sıkıştırma mümkün olan noktaları tespit edip uygular.

---

## Özet: Neleri Daha da Sıkıştırdık?

| Değişiklik | Önceki | Yeni | Token Kazancı |
|---|---|---|---|
| Implicit return | `fn f()->i{r x+1}` | `fn f()->i{x+1}` | -1 per fn |
| Return tipi kısa | `fn f()->i` | `fn f()i` | -1 per fn |
| Self kısaltma | `self.name` | `.name` (metod içinde) | -1 per erişim |
| Auto-import | `use math` (stdlib) | Otomatik | -1 per use |
| Single-line if | `i x>0{action()}` | `i x>0:action()` | -2 (süslü parantez) |
| Chained comparison | `x>=18&&x<=65` | `18<=x<=65` | -1 |
| CRUD macro | 20 satır endpoint | `#[crud]` 1 satır | -19 satır |
| Computed fields | `fn area(self)->f=w*h` | `area:f=.w*.h` | -3 |
| Anonim struct dönüş | `st Stats(...)` ayrı | `-> {min:i max:i}` inline | Ayrı st tanımı yok |
| Kısa method syntax | `fn name(self)->s` | `fn name->s` (self implicit) | -1 per method |

---

## 1. Implicit Return (Örtük Dönüş)

Son ifade otomatik olarak dönüş değeridir. `r` sadece **erken çıkış** için kullanılır.

```
// Önceki
fn add(a:i b:i)->i {
    r a + b
}

// Yeni — `r` gereksiz
fn add(a:i b:i)->i {
    a + b
}

// Single-expression zaten OK
fn add(a:i b:i)->i = a + b
```

**Kazanç**: Proje genelinde yüzlerce `r` token tasarrufu.

---

## 2. Return Tipi `->` Kaldırma

`->` operatörü fonksiyon başına 1 token. Kaldırabiliriz çünkü parser `)` sonrası next token'a bakarak tip mi body mi anlayabilir.

```
// Önceki
fn add(a:i b:i)->i = a+b
fn greet(name:s)->s { "Merhaba {name}" }

// Yeni — -> kaldırıldı
fn add(a:i b:i)i = a+b
fn greet(name:s)s { "Merhaba {name}" }

// Void fonksiyon — tip yok
fn log(msg:s) { print(msg) }
```

**Parser kuralı**: `)` sonrası `{` veya `=` gelirse void, başka bir şey gelirse o return tipi.

**Kazanç**: 1 token × fonksiyon sayısı. 1000 fonksiyonluk projede 1000 token.

---

## 3. Implicit Self — Metod İçinde `.field` Kısayolu

Metod içinde `self.field` yerine `.field` yeterli. **Kendi alanına eriştiğin her yerde** `self` yazman gereksiz.

```
// Önceki
st User(name:s age:i) {
    fn greet(self)->s = "Merhaba {self.name}"
    fn is_adult(self)->b = self.age >= 18
    fn info(self)->s = "{self.name} ({self.age})"
}

// Yeni — self implicit
st User(name:s age:i) {
    fn greet()s = "Merhaba {.name}"
    fn is_adult()b = .age >= 18
    fn info()s = "{.name} ({.age})"
}
```

**Kurallar**:
- Metod tanımlarında `self` parametresi **örtük** — yazmana gerek yok
- `.field` → `self.field` (nokta ile başlayan erişim = self)
- Mutating metod gerekirse: `fn set_name(mut name:s)` → `mut` prefix'i

**Kazanç**: Her metod tanımında -1 parametre, her field erişiminde -4 karakter (`self`). Struct-ağırlıklı projelerde **devasa** tasarruf.

---

## 4. Single-Line If (Tek Satır Koşullu)

Koşullu çalıştırma için süslü parantez gereksiz:

```
// Önceki — her zaman süslü parantez
i x > 0 { action() }
i err != nil { r Err(err) }

// Yeni — iki nokta ile tek satır
i x>0: action()
i err!=nil: r Err(err)

// Çok satırlı blok hala süslü parantezle
i x > 0 {
    step1()
    step2()
}

// if-else tek satır (ternary ile aynı ama statement olarak)
i x>0: action_a() e: action_b()
```

**Kazanç**: 2 token (`{` ve `}`) × basit if sayısı. Projelerde if'lerin %60'ı tek satırlık.

---

## 5. Chained Comparison (Zincir Karşılaştırma)

Matematikteki gibi zincirleme karşılaştırma:

```
// Önceki
i x >= 18 && x <= 65 { ... }
i a < b && b < c { ... }

// Yeni — matematik notasyonu
i 18<=x<=65 { ... }
i a<b<c { ... }

// Karma kullanım
i 0 < len && len <= max_size { ... }
// →
i 0<len<=max_size { ... }
```

**Kazanç**: Her zincir karşılaştırmada 1 operand + 1 operatör (`&&`) + 1 tekrarlanan değişken = 3 token.

---

## 6. Auto-Import (Otomatik İçe Aktarma)

Standart kütüphane **hiçbir zaman import edilmez**. Her şey otomatik erişilebilir.

```
// Önceki
use math
use io
use net.http
v x = math.sqrt(16)
v resp = http.get("https://api.example.com")

// Yeni — use yok, doğrudan kullan
v x = math.sqrt(16)
v resp = http.get("https://api.example.com")
```

**`use` ne zaman gerekli?**
- Sadece 3rd-party paketler için
- İsim çakışması olduğunda alias `use x -> y`

**Kazanç**: Tipik dosyada 3-8 satır `use` ifadesi → 0 satır. Proje genelinde yüzlerce satır.

---

## 7. Computed Fields (Hesaplanan Alanlar)

Struct'larda sürekli hesaplanan değerler için method yazmak yerine inline tanım:

```
// Önceki — ayrı method
st Rect(w:f h:f) {
    fn area()f = .w * .h
    fn perimeter()f = 2 * (.w + .h)
    fn is_square()b = .w == .h
}

// Yeni — computed field
st Rect(w:f h:f) {
    area:f = .w * .h
    perimeter:f = 2*(.w+.h)
    is_square:b = .w==.h
}
```

**Fark**: Computed field `()` olmadan erişilir: `rect.area` vs `rect.area()`. Daha kompakt, daha doğal.

---

## 8. CRUD Macro — Devasa Sıkıştırma

Bir entity için tam CRUD API yazmak yerine tek satır:

```
// Önceki — tam CRUD (20+ satır)
st Product(id:i name:s price:f stock:i category:s)
v products:[Product]=[]

@get("/products")
fn list(category:s?)[Product]=products|>filter(category==nil||.category==category)

@post("/products")
fn create(p:Product)Product{products.push(p);p}

@put("/products/{id}")
fn update(id:i p:Product)Res<Product>{
    v idx=products|>find_idx(.id==id)?
    products[idx]=p;Ok(p)
}

@del("/products/{id}")
fn remove(id:i){products=products|>filter(.id!=id)}

// Yeni — macro ile 2 satır
#[crud("/products" filter:[category])]
st Product(id:i name:s price:f stock:i category:s)
```

`#[crud]` auto-generates:
- `GET /products` (list + filtreleme)
- `POST /products` (create)
- `PUT /products/{id}` (update)
- `DELETE /products/{id}` (delete)
- `GET /products/{id}` (get by id)

**Kazanç**: 20+ satır → 2 satır = **10x sıkıştırma** per entity.

Özel davranış gerektiğinde macro'yu override et:
```
#[crud("/products" filter:[category price_range])]
st Product(id:i name:s price:f stock:i category:s) {
    // Override: Sadece create'i özelleştir
    @post fn create(p:Product)Product {
        validate(p)?
        p.id = next_id()
        products.push(p)
        p
    }
}
```

---

## 9. Anonim Struct Dönüş (Inline Return Types)

Sadece bir fonksiyonun dönüşü için ayrı struct tanımlamak israf:

```
// Önceki — ayrı struct gerekli
st Stats {
    min: i
    max: i
    avg: f
    count: i
}
fn calc_stats(nums:[i])Stats = Stats{
    min:nums|>min max:nums|>max 
    avg:nums|>avg count:nums.len
}

// Yeni — inline anonim struct
fn calc_stats(nums:[i]){min:i max:i avg:f count:i} = {
    min:nums|>min max:nums|>max avg:nums|>avg count:nums.len
}

// Kullanım — destructuring ile
v {min max avg _} = calc_stats(data)
```

**Kazanç**: Ayrı struct tanımı yok, sadece kullanıldığı yerde inline.

---

## 10. Kısa Collection Operasyonları

Çok kullanılan operasyonlar için kısa formlar:

```
// Uzun form
users |> filter(.active) |> map(.name) |> sort |> take(10)

// Kısa form — zincir operatörler
users~filter(.active)~map(.name)~sort~take(10)
```

Hayır — `~` zinciri `|>` kadar açık değil ve 1 token fark. **`|>` kalacak**, ama operasyon isimlerini kısaltalım:

```
// Yaygın operasyonları kısalt
users |> fil(.active)      // filter → fil
      |> map(.name)        // map zaten 3 harf
      |> srt               // sort → srt
      |> tak(10)           // take → tak
      |> rev               // reverse → rev
      |> unq               // unique → unq
      |> flat              // flatten → flat
      |> red(0 |a b| a+b)  // reduce → red
      |> any(.age>18)      // any zaten 3 harf
      |> all(.active)      // all zaten 3 harf
      |> cnt               // count → cnt
      |> grp(.category)    // group_by → grp
      |> zip(other_list)   // zip zaten 3 harf
```

Yok. Bu okunurluğu benim için bile düşürür. `filter`, `sort`, `take` zaten birer token. **Kısaltılmayacak** — burada net kalmalıyız. Bir LLM olarak, `fil` yazdığımda ne olduğunu biliyorum ama `filter` yazdığımda **%100 eminim** — halisünasyon riski sıfır.

> **Karar**: Stdlib fonksiyon isimleri kısaltılmayacak. Kısa keyword'ler (`i`, `f`, `v`) yeterli tasarruf sağlıyor. Fonksiyon isimleri anlamlı kalmalı.

---

## 11. Multi-Line → Single-Line Collapse Kuralları

Blok gövdesi tek ifade ise her yapı tek satıra inebilir:

```
// For — çok satır
f item in list {
    process(item)
}
// For — tek satır
f item in list: process(item)

// While — tek satır
w queue.len>0: handle(queue.pop())

// Match arm — zaten tek satır
m status {
    200 => Ok(body)
    404 => Err("Not found")
    _ => Err("Unknown: {status}")
}

// Struct method — tek satır
st Vec2(x:f y:f) {
    len:f = (.x**2+.y**2)**0.5
    fn add(o:Vec2)Vec2 = Vec2(.x+o.x .y+o.y)
    fn scale(s:f)Vec2 = Vec2(.x*s .y*s)
    fn dot(o:Vec2)f = .x*o.x+.y*o.y
    fn norm()Vec2 = .scale(1/.len)
}
```

---

## 12. Kısa Enum + Pattern Shorthand

```
// Enum tanımlama — zaten kompakt
en Res<T> { Ok(T) Err(s) }
en Opt<T> { Some(T) None }

// Match shorthand — `m` + implicit variable
fn handle(r:Res<i>)s = m r {
    Ok(v) => "Değer: {v}"
    Err(e) => "Hata: {e}"
}

// If-let shorthand — enum destructure + koşul aynı anda
i Ok(val) = get_data(): process(val)
// Eğer get_data() Ok dönerse val'ı al ve process et
// Err dönerse hiçbir şey yapma
```

---

## 13. Trait Kısayolu

```
// Önceki
tr Printable {
    fn to_s(self)->s
}
impl Printable for User {
    fn to_s(self)->s = "{self.name}"
}

// Yeni — inline impl
st User(name:s age:i) impl Printable {
    fn to_s()s = "{.name} ({.age})"
}

// Birden fazla trait
st User(name:s age:i) impl Printable + Serialize + Eq {
    fn to_s()s = "{.name} ({.age})"
    // Serialize ve Eq auto-derive edilebilir → sadece Printable yazılır
}

// Auto-derive daha kısa
#[Eq Hash Ser]
st User(name:s age:i)
```

**Not**: `#[derive(Eq Hash Serialize)]` → `#[Eq Hash Ser]` — `derive()` wrapper gereksiz.

---

## 14. Güncellenmiş Tam Örnek: E-Ticaret API

Tüm optimizasyonlar uygulanmış haliyle:

```
// ===== models.vp =====

#[Eq Ser Hash]
st Product(id:i name:s price:f stock:i cat:s imgs:[s])

#[Eq Ser]
st User(id:i name:s email:s pass_hash:s) {
    fn valid_email()b = .email|>contains("@")
}

#[Eq Ser]
st Order(id:i user_id:i items:[OrderItem] status:s="pending") {
    total:f = .items|>map(.subtotal)|>sum
}

st OrderItem(product_id:i qty:i price:f) {
    subtotal:f = .qty * .price
}

// ===== api.vp =====

#[crud("/products" filter:[cat])]
st Product

#[crud("/users")]
st User

@post("/orders")
fn create_order(user_id:i items:[{product_id:i qty:i}])Res<Order> {
    v order_items = items |> map(|it| {
        v p = products|>find(.id==it.product_id)?
        i p.stock < it.qty: r Err("Stok yetersiz: {p.name}")
        p.stock -= it.qty
        OrderItem(it.product_id it.qty p.price)
    })
    v order = Order(next_id() user_id order_items)
    orders.push(order)
    Ok(order)
}

@get("/orders/{id}")
fn get_order(id:i)Res<Order> = orders|>find(.id==id)

@get("/stats")
fn sales_stats(){total_revenue:f order_count:i top_product:s} = {
    total_revenue: orders|>map(.total)|>sum
    order_count: orders.len
    top_product: orders|>flat_map(.items)|>grp_by(.product_id)|>max_by(.1.len)|>.0
}

// ===== main.vp =====

@main fn start() {
    db.connect("postgres://localhost/shop")?
    serve(":8080")
}
```

### Token Sayımı Karşılaştırması: Bu E-Ticaret API

| Dil | Tahmini Satır | Tahmini Token |
|---|:---:|:---:|
| Java (Spring Boot) | ~350 | ~1400 |
| Python (Flask) | ~150 | ~600 |
| Go (Gin) | ~200 | ~800 |
| Rust (Actix) | ~250 | ~1000 |
| **ViperLang v1** | ~45 | ~180 |
| **ViperLang v2 (bu doküman)** | **~35** | **~130** |

**v1 → v2 kazanç**: ~%28 daha az token

---

## 15. Son Keyword/Syntax Tablosu (v2)

| Öğe | Syntax | Örnek |
|---|---|---|
| Değişken | `v` | `v x = 10` |
| Sabit | `c` | `c PI = 3.14` |
| Fonksiyon | `fn` | `fn add(a:i b:i)i = a+b` |
| Erken return | `r` | `r Err("fail")` |
| If | `i` | `i x>0: action()` |
| Else if | `ei` | `ei x<0: other()` |
| Else | `e` | `e: fallback()` |
| For | `f` | `f x in list: process(x)` |
| While | `w` | `w active: loop_body()` |
| Loop | `l` | `l { ... }` |
| Match | `m` | `m val { 1=>a() _=>b() }` |
| Struct | `st` | `st Point(x:f y:f)` |
| Enum | `en` | `en Dir{N S E W}` |
| Trait | `tr` | `tr Show{fn show()s}` |
| Impl | `impl` | `st X impl Show{...}` |
| Type alias | `tp` | `tp ID = i` |
| Pipe | `\|>` | `x\|>filter\|>map` |
| Error prop | `?` | `get_data()?` |
| Null coal. | `??` | `val ?? 0` |
| Null safe | `?.` | `obj?.field` |
| Self field | `.field` | `.name` (metod içinde) |
| Tek satır blok | `:` | `i x>0: action()` |
| Computed field | `field:T=expr` | `area:f=.w*.h` |
| Derive | `#[...]` | `#[Eq Ser Hash]` |
| CRUD | `#[crud]` | `#[crud("/path")]` |
| Annotation | `@verb` | `@get("/users")` |
| Implicit return | son ifade | `fn f()i{x+1}` |
| Anonim struct | `{f1:T f2:T}` | `fn f(){a:i b:s}` |
| Chain compare | `a<=x<=b` | `0<len<=100` |
| If-let | `i Pat=expr` | `i Ok(v)=f():use(v)` |

---

## 16. v1 vs v2 — Aynı Kod Karşılaştırması

### Struct + Metodlar

```
// v1
st User(name:s age:i) {
    fn greet(self)->s = "Merhaba {self.name}"
    fn is_adult(self)->b = self.age >= 18
}

// v2
st User(name:s age:i) {
    fn greet()s = "Merhaba {.name}"
    is_adult:b = .age>=18
}
```
**Kazanç**: 12 token → 8 token (**%33**)

### Filter + Transform

```
// v1
fn active_adults(u:[User])->[s] {
    r u |> filter(.active && .age >= 18) |> map(.name) |> sort
}

// v2
fn active_adults(u:[User])[s] = u|>filter(.active&&.age>=18)|>map(.name)|>sort
```
**Kazanç**: 18 token → 14 token (**%22**)

### Error Handling

```
// v1
fn process()->Res<s> {
    v data = read_file("input.txt")?
    v parsed = parse(data)?
    r Ok(transform(parsed))
}

// v2
fn process()Res<s> {
    v data = read_file("input.txt")?
    v parsed = parse(data)?
    Ok(transform(parsed))
}
```
**Kazanç**: `r` ve `->` kaldırıldı — 2 token per fonksiyon

---

## Toplam Sıkıştırma Tahmini

| Optimizasyon | Tahmini Kazanç (proje geneli) |
|---|---|
| Implicit return | ~5% |
| `->` kaldırma | ~3% |
| Implicit self + `.field` | ~8% |
| Auto-import | ~4% |
| Tek satır if/for (`:`) | ~6% |
| Computed fields | ~3% |
| CRUD macro | ~10% (API-ağırlıklı projelerde) |
| Derive kısaltma | ~2% |
| Inline impl | ~2% |
| Chain comparison | ~1% |
| **Toplam v1→v2 kazanç** | **~30-35%** |

### Kümülatif Sıkıştırma (diğer dillere göre)

| Karşılaştırma | Oran |
|---|---|
| ViperLang v2 vs Java | **~12-15x** |
| ViperLang v2 vs Go | **~7-8x** |
| ViperLang v2 vs Rust | **~6-7x** |
| ViperLang v2 vs Python | **~4-5x** |
| ViperLang v2 vs JavaScript | **~4-5x** |
| ViperLang v1 vs v2 | **~1.3-1.4x** (ek sıkıştırma) |
