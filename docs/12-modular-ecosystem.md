# ViperLang v3 — Modüler Ekosistem ve Paket Yönetimi

> **Sorun:** ViperLang, E-ticaretten IoT'ye, Web Backend'lerden Oyun Motorlarına kadar her şeyi kapsayan devasa bir hedefe sahip. Ancak bir LLM veya insan, basit bir CLI aracı yazmak istediğinde bilgisayarına 3D render motorunu veya oyun fizik kütüphanesini indirmek zorunda kalmamalıdır. Bu kaynak israfıdır.
>
> **Çözüm (Kullanıcı Önerisi):** Çekirdek (Core) dil ultra-hafif olmalı (<5MB). Farklı domainler (Web, AI, Oyun, IoT) ihtiyaç duyuldukça kurulan "Resmi Domain Paketleri" (Official Domain Packages) şeklinde tasarlanmalıdır.

---

## 1. ViperLang Çekirdeği (The Core)

`viper` komutunu kurduğunuzda bilgisayarınıza inen tek binary (~5MB) şunları içerir:

1. **Derleyici (Lexer, Parser, Bytecode Generator, AST Manipulator)**
2. **ViperVM (Bytecode Runtime ve Reference Counter)**
3. **Standart Kütüphane (stdlib):**
   *   `fs` (Dosya sistemi)
   *   `io` (Giriş/Çıkış)
   *   `math` (Temel matematik)
   *   `regex` (Düzenli ifadeler)
   *   `net.http` (Temel HTTP client/server)
   *   `json` (JSON parse/serialize)

Bu çekirdek, sistem araçları, scriptler ve temel API API'ler yazmak için yeterlidir. Ram tüketimi ~2MB, derleme hızı <5ms'dir.

---

## 2. Resmi Domain Paketleri (Official Domain Packages)

Eğer projeniz saf bir çekirdek projesinden çıkıp spesifik bir alana (domain) giriyorsa, ViperLang paket yöneticisi (`vpm` - Viper Package Manager, çekirdekle birlikte gelir) aracılığıyla sadece o alanın paketini indirirsiniz.

### Neden "Resmi" (Official)?
NPM (JavaScript) veya PyPI (Python) çöplüğüne dönmemek için. Milyonlarca niteliksiz, güvenlikle zafiyeti olan üçüncü parti kütüphane LLM'lerin kafasını karıştırır. ViperLang'da ana alanlar (domainler) dilin yaratıcıları (White-labelled/Official) tarafından yönetilir ve standartlaştırılır.

### Domain Paket Örnekleri

| Komut | Paket Adı | İçerik ve Kullanım Alanı |
|---|---|---|
| `viper add @web` | Viper Web Framework | Yüksek performanslı HTTP routing, WebSocket, CRUD makroları (`#[crud]`), Middleware. |
| `viper add @data` | Viper Data Science | Matris operasyonları, istatistik, veri filtreleme, CSV devasa boyutlarda işleme. (Python Pandas muadili). |
| `viper add @iot` | Viper Embedded & IoT | I2C, SPI, GPIO kontrolcüsü, bit-level manipülasyonlar, düşük bellek profili. |
| `viper add @game` | Viper Game Engine | ECS (Entity-Component-System), temel fizik çarpışmaları, 2D/3D vektör matematiği. |
| `viper add @db` | Viper ORM & DB | PostgreSQL, MySQL, SQLite sürücüleri ve güvenli sorgu derleyicisi. |

---

## 3. Paket (Bağımlılık) Çözümlemesi: `.vpmod` Manifestosu

ViperLang projeleri `toml`, `json` veya `yaml` gibi hantal (ve parse etmesi yavaş, token israf eden) formatlar kullanmaz. ViperLang'ın derleyicisi zaten kendi sözdizimini bildiği için, proje ayarları da doğrudan dilin kendi token-optimize yapısıyla, yani **`.vpmod` (Viper Module Manifest)** formatında tanımlanır.

### Token-Optimize Paket Tanımı (`viper.vpmod`)

```
// viper.vpmod

@project("e_commerce_api" v:1.0)
@author("AI_Agent_001")

// Resmi paketler (Sadece @ ve sürüm)
use @web(1.2)
use @db(2.0)
use @data(1.1)

// Gayriresmi (3rd party) paketler
use github.com/stripe/viper-stripe(1.0) -> stripe
```

**LLM Kazanımları:**
1. **Parser Tekilliği:** Derleyici ekstra bir TOML/JSON parser'ı barındırmak zorunda kalmaz. RAM ve Executable boyutu küçülür. 
2. **Token Tasarrufu:** TOML'daki `[dependencies]`, string tırnakları `""` ve `=` atamaları tamamen yok edildi.
3. **AST Entegrasyonu:** Bu dosya düz bir metin değil, derleyici için statik bir AST node'udur. Paket çözümlemesi anında ve hatasız yapılır.

### Derleme Anında Sadece Kullanılanı Alma (Tree Shaking)
`use @web(1.2)` yazıp indirdiğinizde bile, ViperLang derleyicisi devasa bir build dosyası oluşturmaz. Derleyici AST seviyesinde çalıştığı için, bağladığınız `#[crud]` makrosu `@web` paketinden **sadece** route oluşturma ve JSON parse etme bytecodelarını son `.vpc` (Viper Compiled) dosyasına ekler.
*Kullanılmayan hiçbir özellik RAM'e yüklenmez.*

---

## 4. LLM İçin Paket Yönetiminin Avantajı

1. **Bağlam Tasarrufu (Context Savings):** Eğer ben (LLM) IoT kodlayan bir ajan isem, bana Web Framework'ünün dokümanlarını, sınıflarını ve AST nodelarını yüklemenize gerek kalmaz. Sadece `@iot` paketinin contractlarını okurum.
2. **Güvenlik (Supply Chain Security):** NPM'de bir geliştirici kütüphanesini sildiğinde (Örn: `left-pad` krizi) tüm internet çöker. ViperLang'da `@` işaretli kütüphaneler dilin çekirdek deposuyla onaylanmıştır.

## Sonuç
Senin de belirttiğin gibi, dili her şeyi kapsayan hantal bir canavara dönüştürmek, Python veya Java'nın yaptığı hatayı tekrarlamak olurdu. 

**ViperLang'ın çekirdeği şeffaf, hafif ve keskindir. Kas gücü (Domain Packages) sadece çağrıldığında gelir.**
