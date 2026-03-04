# ViperLang — Vizyon ve Mimari

## Problem

LLM'ler ve AI coding agent'lar:
- **Kısıtlı bağlam penceresinde** (context window) projeler yazıyor
- Proje büyüdükçe **halisünasyon** başlıyor — olmayan API'leri çağırıyor, yanlış imzalar kullanıyor
- Mevcut dillerin **%40-60'ı boilerplate** — sematik değeri sıfır olan token israfı

## Çözüm: ViperLang

**Yalnızca LLM'ler için** tasarlanmış, **native olarak derlenen**, token-optimize bir programlama dili.

### Hedefler

| Hedef | Metrik |
|---|---|
| Sıkıştırma | 100K satır → ≤5K satır (20x) |
| Cold start | <5ms |
| RAM kullanımı | Minimal (MB cinsinden, GB değil) |
| Bağımsızlık | Sıfır dış bağımlılık, tek binary |
| Performans | C'ye yakın çalışma hızı |

### Tasarım İlkeleri

| # | İlke |
|---|---|
| 1 | **Her token maksimum semantik yük taşır** — boş token yok |
| 2 | **İnsan okunurluğu önemsiz** — yoğun, kompakt syntax |
| 3 | **Sıfır boilerplate** — auto-derive, auto-implement |
| 4 | **Yüksek seviye pattern'ler** dil seviyesinde primitif |
| 5 | **Açık modül sınırları** — contract blokları ile API görünürlüğü |
| 6 | **Düz yapı** — iç içe geçme yerine pipe/chain composition |
| 7 | **Tutarlı syntax** — LLM tahmin edilebilirliği maksimum |

---

## Mimari: Native Derleme

ViperLang **kendi başına çalışan, bağımsız bir dildir**. Transpile etmez, başka dillere dönüştürmez.

```
ViperLang kaynak (.vp)
    ↓
Lexer → Token Stream
    ↓
Parser → AST
    ↓
Semantic Analyzer (tip kontrolü, scope çözümleme)
    ↓
Bytecode Compiler → .vpc dosyası (Viper Compiled)
    ↓
ViperVM (ultra-hafif bytecode VM)
    ↓
Çalıştırma
```

### Neden Bytecode VM?

| Alternatif | Cold Start | RAM | Karmaşıklık | Karar |
|---|---|---|---|---|
| Doğrudan makine kodu (AOT) | ~10ms | Çok az | Çok yüksek, platform bağımlı | ❌ |
| LLVM backend | ~100ms+ | Yüksek (LLVM bağımlılığı) | Orta | ❌ |
| JIT compilation | ~50ms (warmup) | Orta-Yüksek | Yüksek | ❌ |
| **Custom bytecode VM** | **<5ms** | **Çok az** | **Orta** | **✅** |
| Tree-walk interpreter | <5ms | Az | Düşük | ❌ Çok yavaş |

**Custom bytecode VM** seçildi çünkü:
1. **Cold start <5ms** — bytecode zaten derlenmiş, VM sadece çalıştırır
2. **Minimal RAM** — VM kendisi ~1-2MB, çalışan kod birkaç MB
3. **Platform bağımsız** — aynı `.vpc` her yerde çalışır
4. **Sıfır bağımlılık** — LLVM, GCC gibi dev bağımlılıklar yok
5. **C'ye yakın hız** — register-based VM ile native'e yakın performans

### Compiler + VM → C ile yazılacak

Neden C:
- **En düşük overhead** — zero-cost abstraction
- **Her platformda derlenebilir** — Linux, macOS, Windows
- **Minuscule binary size** — tek binary <5MB hedefi
- **Doğrudan sistem kaynaklarına erişim** — syscall seviyesi kontrol
- **Bağımlılık yok** — sadece C standard library

### Build ve Dağıtım

```
# Kullanıcı deneyimi:
$ viper build main.vp        # → main.vpc (bytecode)
$ viper run main.vp           # → derle + çalıştır (tek adım)
$ viper main.vp               # → kısayol, aynı şey

# Kurulum: tek binary
$ curl -sSL viper.dev/install | sh
# veya
$ brew install viper
```

Kullanıcı tek bir binary yükler, hepsi bu. İçinde:
- Lexer + Parser + Compiler
- VM Runtime
- Standart kütüphane
- Paket yöneticisi

---

## Bellek Yönetimi: Reference Counting + Escape Analysis

GC (Garbage Collection) çok fazla RAM ve pauz süresi yaratır. Bunun yerine:

### Reference Counting (RC)
- Her nesne bir referans sayacı taşır
- Sayaç 0'a düştüğünde anında serbest bırakılır
- **Deterministik** — ne zaman bellek serbest kalacağı önceden bilinir
- **Düşük overhead** — GC pause yok

### Cycle Detection (Döngüsel Referans)
- Compile-time analiz ile çoğu döngü tespit edilir
- Geriye kalan nadir durumlar için hafif bir cycle collector
- RC + cycle detection = Swift'in modeli (kanıtlanmış, verimli)

### Escape Analysis
- Compiler değişkenin scope dışına çıkıp çıkmadığını analiz eder
- **Kaçmayan değişkenler stack'te kalır** — heap allocation yok
- Stack allocation = sıfır maliyet (RAM açısından)

### Arena Allocation (Opsiyonel)
- Request-scoped veriler için arena allocator
- Tüm arena tek seferde serbest bırakılır
- Web server gibi senaryolarda mükemmel

### Sonuç
```
Stack allocation (escape etmeyen)     → Sıfır maliyet
Reference counting (heap nesneleri)   → Deterministik, düşük overhead  
Cycle collector (nadir durumlar)      → Lazy, hafif
Arena allocator (request-scoped)      → Toptan serbest bırakma
```
**Tahmini RAM kullanımı**: Eşdeğer Go/Python programının **1/5 ile 1/10'u**.

---

## Token Verimliliği Karşılaştırması

### Örnek: REST API Endpoint

#### Python + Flask (~85 token)
```python
from flask import Flask, jsonify, request
from dataclasses import dataclass
from typing import List

app = Flask(__name__)

@dataclass
class User:
    id: int
    name: str
    email: str
    active: bool = True

users: List[User] = []

@app.route('/users', methods=['GET'])
def get_users():
    active = request.args.get('active', 'true') == 'true'
    result = [u for u in users if u.active == active]
    return jsonify([vars(u) for u in result])

@app.route('/users', methods=['POST'])
def create_user():
    data = request.get_json()
    user = User(**data)
    users.append(user)
    return jsonify(vars(user)), 201
```

#### ViperLang (~20 token) ← HEDEF
```
st User(id:i name:s email:s active:b=true)

v users:[User]=[]

@get("/users")
fn list(active:b=true)->[User]=users|>filter(.active==active)

@post("/users")
fn create(data:User)->User{users.push(data);r data}
```

**4x daha az token**, aynı işlevsellik.

---

## Hedef Kullanıcılar

| Kullanıcı | Rol |
|---|---|
| **LLM / AI Agent** | Kodu yazar, okur, değiştirir (birincil kullanıcı) |
| **İnsan geliştirici** | `viper` binary'sini yükler, agent'a "kodla" der |
| **ViperVM** | Bytecode'u çalıştıran ultra-hafif VM |

---

## Dosya Uzantıları

| Uzantı | Açıklama |
|---|---|
| `.vp` | ViperLang kaynak dosyası |
| `.vpc` | Derlenmiş bytecode |
| `.vpmod` | Modül manifest dosyası |
