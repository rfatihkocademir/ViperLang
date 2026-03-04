# ViperLang v3 — Milyar Satırları Bağlama Sığdırmak (Extreme Compression)

> **Hedef:** Gerçek dünyadaki 1.000.000.000 (Bir Milyar) satırlık hantal bir legacy projeyi (Java/C++ vb.), ViperLang'ın aşırı sıkıştırma gücüyle, **tek bir LLM'in bağlam penceresine (context window) sığacak boyuta (örn. 50-100K satır)** indirgemek.
> 
> **Yanlış Anlaşılmayı Düzeltme:** Amacımız 1 milyar satır ViperLang kodu *yazmak* değildir. Amacımız, 1 milyar satırlık bir problemi öylesine yoğun ve strict (katı) bir şekilde ifade etmektir ki, o devasa sistem **tamamen senin (LLM'in) zihninin içine** tek seferde sığabilsin.
>
> Eğer sıkıştırılmış kod hala bağlam penceresinden taşıyorsa, RAG (Retrieval-Augmented Generation) sisteminin %100 kesinlikle çalışmasını sağlayacak O(1) semantik kurallar işletilir.

---

## 1. RAG-First Syntax ve Mutlak Çözünürlük

### Kural: Küresel Benzersiz Kimlikler (Globally Unique Identifiers)
Bir obje veya fonksiyona referans verilirken, arka planda hangi modülden geldiği asla "tahmin" edilmemelidir. 

**ViperLang v3 Çözümü:** İsim çakışmaları (name collision) tamamen yasaklanmıştır.
*   Aynı projede iki farklı `User` struct'ı olamaz. `db.User` ve `api.User` gibi kavramlar yoktur.
*   Her struct ve trait projenin tamamında **benzersiz** bir isme sahip olmak zorundadır (örneğin: `DbUser`, `ApiUser`).
*   **Neden?** Çünkü RAG sistemine "User struct'ını getir" dediğimde bana 50 farklı dosya getirmemeli. O(1) kesinlikte tek bir dosya getirmelidir.

---

## 2. Etki Sistemi (Effect System): Blast Radius Sıfırlama

Bir LLM olarak 1 milyar satırlık eşdeğer bir sistemin ViperLang versiyonunda bir fonksiyonu değiştirdiğimde, **sistemin başka hiçbir yerinin kırılmayacağından %100 emin olmalıyım**.

### Kural: Explicit Mutation Contracts (Açık Mutasyon Sözleşmeleri)
Eğer bir fonksiyon argüman olarak aldığı bir objeyi değiştiriyorsa (mutate), bunu sadece `mut` anahtar kelimesiyle değil, **objenin hangi alanlarını (fields) değiştirdiğini de fonksiyon imzasında belirtmek zorundadır.**

```
// Eski (v2) - Yetersiz
fn update_user(mut u:User) {
    u.status = "active"
    u.login_count += 1
}

// YENİ (v3) - Etki beyanı zorunlu
fn update_user(mut u:User[status, login_count]) {
    u.status = "active"
    u.login_count += 1
}
```

**LLM Kazanımı (Blast Radius Control):**
Derleyici, benim belirttiğim alanlar (`status`, `login_count`) haricinde `u` objesine dokunmama izin vermez. 
Daha da önemlisi, 1 milyar dolarlık/satırlık bir legacy projeyi sıkıştırdığımızda, *"User objesinin 'email' alanını değiştiren tüm fonksiyonları bul"* dediğimde, parser AST üzerinden bana **O(1) hızda kesin bir liste verir**. Full-text search (grep) yapmama gerek kalmaz.

---

## 3. Katı Modül Kuralları (Enforced Modularity)

Kod tabanı milyarlarca satıra ulaştığında "spaghetti kod" LLM'ler için bile takip edilemez bir kabusa dönüşür.

### Kural: The 500-Token Module Limit
Hiçbir dosya (modül) 500 satırı / 5000 token'ı geçemez. Derleyici bu sınırı aştığınızda **derlemeyi reddeder**.
*   **Neden?** Çünkü LLM'in o dosyayı incelemesi gerektiğinde, dosyanın tamamının küçük ve ucuz bir bağlam penceresine (context window) her zaman sığması garanti edilmelidir.

### Kural: Sıfır Döngüsel Bağımlılık (Zero Circular Dependency)
A modülü B'yi, B modülü A'yı include edemez. Derleyici anında patlar. Hiçbir istisnası yoktur (interface veya trait ile workaround yapmak bile yasaklanmıştır). Sistem katı bir Directed Acyclic Graph (DAG) olmalıdır.

---

## 4. O(1) Semantic Resolution (Semantik Çözünürlük)

JavaScript'te `a.process()` kodunda `a`'nın tipini bilmek için bazen programı çalıştırmak gerekir. Bu durum LLM halisünasyonlarının bir numaralı kaynağıdır.

### Kural: Type Mismatch is FATAL (Tip Çıkarımının Sınırlandırılması)
v2'deki "güçlü tip çıkarımını" iptal ediyoruz. **Aşırı büyük projelerde Implicit Type Inference zararlıdır.** 

```
// YASAK: LLM veya RAG sistemi 'x'in tipini bulmak için 'get_data' fonksiyonunun içine girmek zorundadır.
v x = get_data() 

// ZORUNLU (v3): Her değişkenin tipi açıkça yazılmak zorundadır.
v x:UserData = get_data()
```

**LLM Kazanımı:** Sadece tek bir satıra bakarak o değişkenin ne olduğunu bilebilirim. RAG ile sadece o satırı çektiğimde hiçbir bağlam kaybı yaşamam.

---

## 5. Shadow state (Gizli Durum) İmhası

### Kural: No Implicit Context (Context Objelerinin Taşınması)
Go veya React gibi dillerde arka planda sessizce taşınan `context` objeleri vardır. ViperLang v3'te hiçbir şey sessizce taşınamaz. Bir fonksiyon HTTP isteği (request) okuyorsa, DB bağlantısı kullanıyorsa, imzası bunu bağırmak zorundadır.

```
// YASAK (Büyülü Context)
fn fetch_user(id:i)Res<User> {
    v db = get_current_db_conn() // Bu nereden geldi? Halisünasyon sebebi!
    db.query("...")
}

// ZORUNLU (v3) - Dependency Enjeksiyonu imzada olmak zorunda
fn fetch_user(db:DbConn id:i)Res<User> {
    db.query("...")
}
```

---

## 6. Milyar Satır Ölçeğinde "Refactoring" Güvenliği

LLM olarak milyar satırlık bir projede bir API'nin imzasını değiştirmem istendiğinde, binlerce dosyada hata patlayabilir.

### Kural: Deprecated & Migrate-Next (Derleyici Seviyesinde Göç)
Bir fonksiyonu değiştiremem. Sadece `v2`'sini yazabilirim. Derleyici, eski kodları yeniye dönüştürmem için bana (LLM'e) AST düzeyinde bir `#[migrate]` makrosu sunar.

```
#[deprecated(use="fetch_user_v2")]
#[migrate(|old_call| -> "fetch_user_v2(old_call.args[0], default_timeout)")]
fn fetch_user(id:i)Res<User>
```

Ben (LLM) projeye `viper run-migrations` dediğimde, projede sıkıştırılmış olan devasa mimarinin her yerindeki o eski fonksiyon çağrıları, benim belirttiğim AST transformasyon kuralına göre **%100 güvenli bir şekilde** yeniden yazılır. String replace (regex) değil, AST replace yapılır.

---

## Sonuç: ViperLang v3 Manifestosu

1. İnsan konforu sıfırdır.
2. Yazım (typing) fazladır (Explicit types, Explicit mutations).
3. "Zekice" (clever) kod yasaktır. 
4. Her satır kod, bağlamından tamamen koparılıp uzay boşluğuna atılsa bile (RAG chunking), `%100` semantik anlam ifade etmek zorundadır.

**ViperLang, bir programlama dili olmaktan çıkıp, dünyayı yönetecek Yapay Zeka orkestraları için yaratılmış mutlak bir Matematiksel Model (Calculus) haline gelmiştir.**
