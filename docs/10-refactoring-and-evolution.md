# ViperLang v3 — Refactoring ve Özellik Yönetimi (Feature Toggles)

> **Amaç:** Bir LLM olarak milyarlarca satırlık bir projeye yeni bir özellik eklerken veya eskimiş bir özelliği çıkarırken, **hiçbir yeri kırmadan (zero-breakage)** bu işlemi yapabilmemi sağlayan derleyici (compiler) seviyesindeki araçları tanımlamak.

Gerçek dünyada kod yazılmaz, kod *değiştirilir*. ViperLang, refactoring işlemini bir metin düzenleme (string replace) problemi olmaktan çıkarıp, matematiksel olarak doğrulanabilir bir AST (Abstract Syntax Tree) dönüşümü haline getirir.

---

## 1. Feature Toggle (Özellik Aç/Kapat) Sistemi

Geleneksel dillerde bir özelliği kapatmak için her yere `if (FEATURE_ENABLED)` if'leri saçılır. Bu durum kodu kirletir ve ölü kod (dead code) birikmesine yol açar.

ViperLang'da özellik yönetimi derleyici (compile-time) seviyesindedir.

### Kural: AST-Level Feature Flags
Bir modül, fonksiyon veya struct doğrudan bir "Flag" arkasına saklanabilir.

```
// api.vp
#[feature("new_payment_gateway")]
fn process_payment(mut cart:Cart[status])Res<()> {
    // ... yeni entegrasyon
}

// Fallback (eski sistem) - Eğer bayrak kapalıysa bu derlenir
#[fallback("new_payment_gateway")]
fn process_payment(mut cart:Cart[status])Res<()> {
    // ... eski legacy sistem
}
```

**LLM Kazanımı:** Müşteri bana "Yeni ödeme sistemini kapat, eskisine dön" dediğinde, kodun içindeki `if` bloklarını tek tek bulup silmem gerekmez. Sadece konfigürasyondan `new_payment_gateway = false` yaparım. Derleyici AST'den yeni fonksiyonu tamamen siler ve fallback fonksiyonunu yerleştirir. Bağlamım (context) tertemiz kalır.

---

## 2. Güvenli Özellik Silme (Safe Deletion)

Milyar satırlık bir projede bir fonksiyonu veya struct'ı silmek kabustur. "Acaba bunu kim kullanıyordu?" korkusuyla ölü kodlar yıllarca projede kalır.

ViperLang'da kod silinmez, **zehirlenir (poisoned)**.

### Kural: The `@poison` Directive
Bir özelliği kaldırmak istediğimde onu silmem. Onu zehirlerim.

```
// Eski, kullanılmamasını istediğim fonksiyon
#[poison(reason="Use checkout_v2 macro instead", replace="checkout_v2")]
fn legacy_checkout(id:i)Res<()> {
    // ...
}
```

Zehirli bir fonksiyonu çağıran kodlar anında derleme hatası verir. Ancak daha da önemlisi, derleyici bana (LLM'e) tam olarak hangi dosyalarda, hangi satırlarda bu zehirli fonksiyonun çağrıldığının **kesin listesini** verir.

**Geleneksel dillerle farkı:** `Deprecated` sadece bir uyarıdır (warning) ve genellikle görmezden gelinir. `@poison` derlemeyi kırar (fatal error) ve LLM'e o kodu silmesi/değiştirmesi için net, çözülebilir bir yapılacaklar listesi (AST-path list) sunar.

---

## 3. AST Dönüşümleri (AST Transformations)

Bir struct'ın alan adını değiştirmek veya bir fonksiyona yeni bir parametre eklemek istediğimde bunu metin arama-değiştirme (regex) ile yapmam.

### Derleyiciye AST Talimatı Vermek
Diyelim ki `User` struct'ına `email` alanı eklemek istiyorum. Bunu LLM olarak ben manuel yaparsam hata yapabilirim. Bunun yerine ViperLang derleyicisine kendi diliyle "Göç (Migration)" yazarsam:

```
// migration_001.vpmod (Derleme anında çalışan geçici modül)
@migration
fn add_email_to_user() {
    // Projedeki tüm User nesnelerini bul
    v nodes = ast.find_struct("DbUser")
    
    // Email alanını ekle
    nodes.add_field("email", "s", "\"unknown@example.com\"")
    
    // struct'ı başlatan her yere default değeri argüman olarak ekle
    ast.find_calls("DbUser").add_arg("email", "\"unknown@example.com\"")
}
```

Ben `viper migrate migration_001.vpmod` komutunu çalıştırdığımda, derleyici milyarlarca satır kodu okur, AST seviyesinde bu değişikliği yapar ve kodu geri yazar (formatlar). 

**Neden Mükemmel?** Ben (LLM) milyar satır kodu regex ile değiştirmeye çalışırsam %1 ihtimalle de olsa bir yerde yanlış virgül koyarım. Ama değişikliği AST üzerinden derleyici yaparsa, doğruluk oranı **%100'dür**.

---

## 4. The "Hollow" Pattern (Boşaltma Deseni)

Bir kütüphaneyi veya modülü tamamen projeden çıkarmak istediğimde (örneğin eski bir loglama sistemini), o sistemin kullanıldığı on binlerce yeri silmem gerekir.

ViperLang'da bunu "Hollow" (İçini Boşaltma) ile yaparım:

```
// Eski loglama modülü (logger.vp)
// Projeden çıkarılacak ama 50.000 yerde 'logger.info()' çağrısı var

#[hollow]
pub mod logger {
    // Derleyici bu modüldeki tüm fonksiyonların içini boşaltır 
    // ve hiçbir uyarı vermeden bu satırları optimize eder (siler).
    
    pub fn info(msg:s) {}  // İçi derleyici tarafından otomatik silinir
    pub fn error(msg:s) {} // İçi derleyici tarafından otomatik silinir
}
```

`#[hollow]` bayrağını eklediğim an, projedeki 50.000 adet `logger.info()` çağrısı derleme aşamasında (AST optimizasyonunda) **hiçliğe (no-op)** dönüşür. Benim (LLM'in) 50.000 dosyayı tek tek gezip o satırları silmeme gerek kalmaz.

---

## Özet: Refactoring Artık Bir Metin Problemi Değil

ViperLang v3'te kod bir metin (string) dosyası değildir; derleyicinin anladığı ve benim (LLM'in) derleyici aracılığıyla manipüle edebildiğim sağlam bir **Ağaç (Tree)**'dir. 

*   Özellik ekle/çıkar: **Feature Flags**
*   API Değiştir: **AST Migrations**
*   Eski Koddan Kurtul: **Zehrin (Poison) Gücü**
*   Bağımlılık Yok Et: **Hollow Pattern**

Bu araçlar sayesinde, bir insanın 2 ayda korkarak yapabileceği devasa bir refactoring işlemini, ben bir LLM olarak 5 saniye içinde, **sıfır hata ile** ve matematiksel kesinlikle tamamlayabilirim.
