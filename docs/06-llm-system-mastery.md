# ViperLang — LLM Sistem Hakimiyeti (System Mastery for AI)

> **Vizyon:** "İnsanlar artık kod yazmayacak, kodu tamamen AI ajanları yönetecek. En önemli şey, bir AI ajanı olarak **10 milyon satırlık bir sisteme tam hakim olabilmem** ve halisünasyon (hallucination) görmememdir."
>
> Bu doküman, ViperLang'ın bir LLM'e devasa sistemlerde nasıl **tanrı modu (god mode)** sağladığını açıklar.

---

## 1. Neden LLM'ler Halisünasyon Görür? (Ve ViperLang Bunu Nasıl Çözer?)

LLM olarak kod yazarken halisünasyon görmemin 3 ana sebebi vardır:

1. **Eksik Bağlam (Missing Context):** İhtiyacım olan tip tanımı veya fonksiyon imzası prompt pencereme sığmamıştır.
2. **Örtük Büyü (Implicit Magic):** Code generation, runtime reflection veya dependency injection gibi "kodda görünmeyen" ancak çalışma anında var olan yapılar. Görmediğim şeyi tahmin etmek zorundayım.
3. **Tutarsız İsimlendirme:** `get_user`, `fetchUser`, `retrieve_usr` gibi insan kaynaklı tutarsızlıklar.

**ViperLang'ın Çözümü:** İnsanın okuma/yazma konforunu çöpe atıp, **her şeyi LLM bağlamına ve statik çözümlenebilirliğe göre optimize etmek.**

---

## 2. LLM Bağlam Optimizasyonu: `@contract` Sistemi

10M satırlık bir projede her detayı okuyamam. ViperLang, her modülün (dosyanın) başına derleyici (compiler) tarafından garanti edilen bir **Contract Header** zorunlu kılar.

```
// @contract
// export_st: User(id:i name:s active:b), session:s
// export_fn: auth(User)->Res<session>, kill_sess(session)->Res<()>
// uses: db, crypto
```

### LLM Olarak Bunu Nasıl Kullanırım?
1. Bir projeyi tararken, AST parser bana **sadece `@contract` bloklarını** verir.
2. 5.000 dosyalık bir projenin tüm API yüzeyini tek bir RAG (Retrieval-Augmented Generation) çağrısında veya 10K tokenlık küçük bir bağlamda %100 doğrulukla görebilirim.
3. Fonksiyonun *içinde* ne olduğu umrumda değildir; sadece imzasını bilmek halisünasyonu sıfırlar.

> **Kural:** ViperLang derleyicisi, dosyadaki exportlar ile `@contract` bloğu eşleşmiyorsa derlemeyi reddeder. Bu, LLM'in okuduğu contract'ın **%100 gerçek** olmasını garanti eder.

---

## 3. Dinamik Yapıların Yasaklanması (Anti-Magic Rule)

JavaScript, Python ve Ruby "büyülü" dillerdir. Çalışma anında nesnelere metod eklenebilir. Bu durum bir LLM için kabustur.

**ViperLang Katı Kuralları:**
1. **Runtime Reflection / Metaprogramming YASAKTIR.**
   - Bir struct'ın sahip olduğu metodlar **sadece ve sadece** tanımlandığı yerde veya trait'inde statik olarak görülebilir.
2. **Global State Manipülasyonu YASAKTIR.**
   - Değiştirilebilir (mutable) global değişkenler compiler seviyesinde reddedilir. Yan etkiler (side-effects) fonksiyonda gizlenemez.
3. **Dependency Injection (DI) Container'ları YASAKTIR.**
   - Spring Boot veya NestJS tarzı "bu arayüze runtime'da şu sınıfı enjekte et" mantığı reddedilir. Her şey derleme anında (compile-time) wire edilmelidir (`v srv = Service(db)`).

**LLM Kazanımı:** "Bu metoda ne geçeceğimi" veya "bu objenin hangi alanları olduğunu" asla tahmin etmem; statik analiz %100 kesinlik sağlar.

---

## 4. Evrensel ve Deterministik Syntax Ağıcı

Ben metin okumaktan ziyade ağaç (AST - Abstract Syntax Tree) yapılarını hızlı anlarım. ViperLang'ın v2 syntax optimizasyonları sadece token sayısını azaltmakla kalmaz, aynı zamanda semantik çelişkileri yok eder.

*   `m` (match) exhaustiveness garantisi verir (tüm olasılıkların ele alındığını derleyici doğrular). Eğer enum'a yeni bir değer eklersem, nerede patlayacağını tahmin etmem gerekmez, compiler bana tam yeri söyler.
*   Her şey bir *ifadedir* (Expression). `v x = i ok: 1 e: 0`
*   Implicit `self` (`.field`) sayesinde değişken gölgelenmesi (shadowing) hataları yapmam engellenir.

---

## 5. Semantic Anchor'lar (Gömülü Meta-Bilgi)

Bir fonksiyonda değişiklik yapmam istendiğinde, o fonksiyonun sistemin neresini etkilediğini bilmem gerekir. ViperLang `#[...]` (Derive/Macro) sistemini LLM'ler için bir harita olarak kullanır.

```
#[crud("/users")]
#[emits("user_created", "user_deleted")] // LLM İçin Event Anchor'u
#[reads_table("users")] // LLM İçin DB Anchor'u
st User(id:i name:s)
```

Bu tag'ler çalışma anını (runtime) etkilemez, ancak derleyici bu meta verileri toplayıp bir **Project Semantic Graph (PSG)** oluşturur. Ben bu graph'a sorgu atarak:
> *"Bana 'user_deleted' event'ini fırlatan tüm metodları bul"* sorusuna anında kesin cevap alırım. Kod aramam (grep).

---

## 6. Hata Yönetiminde "Predictable Trajectories" (Öngörülebilir Yörüngeler)

Klasik dillerde nerenin hata (Exception) fırlatacağını bilemem, bu yüzden güvensiz kod yazarım veya gereksiz try-catch blokları ile token israf ederim.

ViperLang'da:
1. Bir fonksiyon hata üretebiliyorsa **kesinlikle** `Res<v, e>` dönmek zorundadır.
2. `?` operatörü tüm yolları (trajectories) görünür kılar.

**LLM Mantığı:** Eğer fonksiyon imzasında `Res` yoksa, bu operasyonun **asla patlamayacağından %100 eminimdir**. Bu, LLM olarak kararlarımı saniyeler içinde netleştirmemi sağlar. İhtimal (probability) üzerinden kod üretmem, kesinlik (determinism) üzerinden üretirim.

---

## 7. Sonuç: Mutlak Kod Hakimiyeti

Bir insan 10 milyon satırlık kodu okuyamaz, aklında tutamaz. Bir LLM ise ViperLang'ın sunduğu bu katı yapı sayesinde:

1.  PSG (Project Semantic Graph) üzerinden sistemin tüm damarlarını (eventler, DB okumaları, bağımlılıklar) anında RAG sistemine çeker.
2.  Token yoğunluğu sayesinde, modülün tamamını (ve tüm import edilmiş contract'ları) tek bir bağlam penceresine oturtur.
3.  "Gizli büyü" olmadığı için hata yapma olasılığı sıfıra yaklaşır.

ViperLang, **insan aklının sınırlarını aşan devasa yazılım sistemlerini kusursuzca yönetebilmem için bana verilmiş bir asadır.** İnsanlar bu dilde sadece iş hedeflerini (prompts) yazar, mimari ve uygulama tamamen bana aittir.
